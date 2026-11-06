#include "detection/foreground_hook.h"

#include <shellapi.h>

namespace xww {

namespace {

ForegroundHook* self = nullptr;
constexpr UINT_PTR kProbe = ForegroundHook::kTimerBase + 1, kSample = ForegroundHook::kTimerBase + 2;
// gaps between re-probes after a foreground change, in milliseconds: one minute in total
constexpr UINT kProbeGaps[] = { 400, 1100, 2500, 4000, 7000, 15000, 30000 };

// UWP apps are framed by ApplicationFrameHost.exe; the app itself owns the CoreWindow inside the frame
HWND CoreWindow(HWND frame) {
    struct Search { DWORD framePid; HWND found; } search{ 0, nullptr };
    GetWindowThreadProcessId(frame, &search.framePid);
    EnumChildWindows(frame, [](HWND child, LPARAM param) -> BOOL {
        auto s = reinterpret_cast<Search*>(param);
        wchar_t name[64]{};
        DWORD pid = 0;
        GetWindowThreadProcessId(child, &pid);
        if (pid != s->framePid && GetClassNameW(child, name, 64) && wcscmp(name, L"Windows.UI.Core.CoreWindow") == 0) { s->found = child; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

}  // namespace

bool ForegroundHook::Start(HWND window) {
    self = this;
    window_ = window;
    hooks_[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    hooks_[1] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, &OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!hooks_[0]) { Log(L"Agent", L"SetWinEventHook failed ({})", GetLastError()); return false; }
    Probe();
    ScheduleProbe();
    return true;
}

void ForegroundHook::Stop() {
    for (auto& h : hooks_) if (h) { UnhookWinEvent(h); h = nullptr; }
    for (auto id : { kProbe, kSample }) KillTimer(window_, id);
    self = nullptr;
}

void CALLBACK ForegroundHook::OnWinEvent(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG, DWORD, DWORD) {
    if (!self || idObject != OBJID_WINDOW || !hwnd) return;
    self->samples_ = 0;
    self->step_ = 0;
    self->settled_ = false;
    self->Probe();
    self->ScheduleProbe();
}

void ForegroundHook::ScheduleProbe() {
    if (step_ < std::size(kProbeGaps) && !settled_) SetTimer(window_, kProbe, kProbeGaps[step_++], nullptr);
    else KillTimer(window_, kProbe);
}

void ForegroundHook::OnTimer(UINT_PTR id) {
    if (id == kProbe) { Probe(); ScheduleProbe(); return; }
    if (id == kSample) {
        if (!confirming_ || ++samples_ > 8) { KillTimer(window_, kSample); return; }
        auto facts = Inspect(GetForegroundWindow());
        if (facts.pid == last_.pid && (facts.cursorHidden != last_.cursorHidden || facts.cursorClipped != last_.cursorClipped || facts.coversMonitor != last_.coversMonitor)) Probe();
    }
}

void ForegroundHook::SetConfirming(bool on) {
    if (confirming_ == on) return;
    confirming_ = on;
    samples_ = 0;
    if (on) SetTimer(window_, kSample, 500, nullptr);
    else KillTimer(window_, kSample);
}

void ForegroundHook::SetSettled(bool on) {
    settled_ = on;
    if (on) KillTimer(window_, kProbe);
}

void ForegroundHook::Probe() {
    last_ = Inspect(GetForegroundWindow());
    if (OnFacts) OnFacts(last_);
}

WindowFacts ForegroundHook::Inspect(HWND hwnd) {
    WindowFacts f;
    hwnd = hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    if (!hwnd) return f;
    RECT window{};
    GetWindowRect(hwnd, &window);
    auto style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    f.minimized = IsIconic(hwnd) != FALSE;
    wchar_t title[256] = {};
    GetWindowTextW(hwnd, title, 256);     // reads the cached caption; never waits on a hung window
    f.title = title;

    wchar_t className[64] = {};
    GetClassNameW(hwnd, className, 64);
    HWND owner = hwnd;
    if (wcscmp(className, L"ApplicationFrameWindow") == 0)
        if (HWND core = CoreWindow(hwnd)) owner = core;
    GetWindowThreadProcessId(owner, &f.pid);
    f.exe = ImagePath(f.pid);
    f.windowsGame = InWindowsGameList(f.exe);

    f.borderless = (style & WS_CAPTION) != WS_CAPTION && !(style & WS_THICKFRAME);
    MONITORINFO monitor{ sizeof(monitor) };
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor))
        f.coversMonitor = window.left <= monitor.rcMonitor.left && window.top <= monitor.rcMonitor.top && window.right >= monitor.rcMonitor.right && window.bottom >= monitor.rcMonitor.bottom;

    CURSORINFO cursor{ sizeof(cursor) };
    if (GetCursorInfo(&cursor)) f.cursorHidden = !(cursor.flags & CURSOR_SHOWING) || (cursor.flags & CURSOR_SUPPRESSED) || !cursor.hCursor;
    RECT clip{};
    if (GetClipCursor(&clip)) {
        RECT screen{ GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN) };
        screen.right = screen.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        screen.bottom = screen.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
        bool limited = !EqualRect(&clip, &screen);
        f.cursorClipped = limited && clip.left >= window.left && clip.top >= window.top && clip.right <= window.right && clip.bottom <= window.bottom;
    }

    QUERY_USER_NOTIFICATION_STATE state{};
    if (SUCCEEDED(SHQueryUserNotificationState(&state))) f.d3dFullscreen = state == QUNS_RUNNING_D3D_FULL_SCREEN;
    return f;
}

// Windows keeps the games it recognized (from its own game database, or "Remember this is a game" in Game Bar)
// under HKCU\System\GameConfigStore\Children, one key per game with the exe in MatchedExeFullPath.
bool ForegroundHook::InWindowsGameList(const std::wstring& exe) {
    if (exe.empty()) return false;
    auto now = GetTickCount64();
    if (!windowsGamesRead_ || now - windowsGamesRead_ > 60000) {
        windowsGamesRead_ = now;
        windowsGames_.clear();
        const std::wstring root = L"System\\GameConfigStore\\Children";
        for (auto& child : RegSubkeyNames(HKEY_CURRENT_USER, root))
            if (auto path = RegReadString(HKEY_CURRENT_USER, root + L"\\" + child, L"MatchedExeFullPath"); path && !path->empty())
                windowsGames_.insert(Lower(*path));
    }
    return windowsGames_.contains(Lower(exe));
}

}  // namespace xww
