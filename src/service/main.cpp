//  xwwPulseService.exe, the Core. Runs as a LocalSystem service in session 0:
//  ETW, the detection pipeline, every privileged change and the journal.
//
//    xwwPulseService.exe                 run as a service (started by the SCM)
//    xwwPulseService.exe --install       register the service and the dashboard at sign-in, start both
//    xwwPulseService.exe --uninstall     undo everything, remove the service
//    xwwPulseService.exe --console       run in this console (debugging; needs the dashboard for foreground facts)
//    xwwPulseService.exe --detect-only   like --console, but only logs what it would do
//    xwwPulseService.exe --etw-test [s]  print the busiest frame presenters for s seconds; changes nothing
//    xwwPulseService.exe --recover       replay the journal and exit
#include "core/ipc.h"
#include "core/journal.h"
#include "detection/detector.h"
#include "optimizer/optimizer.h"

#include <conio.h>
#include <cstdio>
#include <deque>
#include <userenv.h>
#include <wtsapi32.h>

using namespace xww;

namespace {

SERVICE_STATUS_HANDLE statusHandle = nullptr;
SERVICE_STATUS status{ SERVICE_WIN32_OWN_PROCESS, SERVICE_START_PENDING };
Handle stopEvent, wakeEvent;
class Core* running = nullptr;
bool asService = false;

void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0) {
    if (!statusHandle) return;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = exitCode;
    status.dwWaitHint = waitHint;
    status.dwControlsAccepted = state == SERVICE_START_PENDING ? 0 : SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_PRESHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE;
    SetServiceStatus(statusHandle, &status);
}

// ------------------------------------------------------------------ core

class Core {
public:
    explicit Core(bool dryRun) : dryRun_(dryRun) {}

    int Run() {
        running = this;
        Optimizer::Recover();
        Config::WriteDefaultsIfMissing();
        config_ = Config::Load();
        EnablePrivilege(SE_INC_BASE_PRIORITY_NAME);
        Log(L"Core", L"xwwPulse {} started{}; {}", XWW_VERSION, dryRun_ ? L" (detect-only)" : L"", TopologyText());

        optimizer_.SetDryRun(dryRun_);
        optimizer_.SetConfig(config_);
        detector_.SetConfig(config_);
        SyncSignIn();
        Wire();
        if (!etw_.Start()) Log(L"Core", L"ETW is unavailable; detection falls back to window and file heuristics");
        server_.Start();

        // config.toml and background.toml edited by hand are picked up without a restart
        Handle watch(FindFirstChangeNotificationW(ProgramDataDir().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE));
        ULONGLONG lastTick = 0, lastStatus = 0;
        if (asService) agentCheckAt_ = GetTickCount64() + kAgentGraceMs;
        for (;;) {
            DWORD timeout = detector_.Confirming() ? detector_.TickInterval() : (optimizer_.Engaged() && !optimizer_.Paused()) ? 1000 : INFINITE;
            for (ULONGLONG at : { pauseAt_.load(), agentCheckAt_.load() })
                if (at) timeout = std::min<DWORD>(timeout, static_cast<DWORD>(at > GetTickCount64() ? at - GetTickCount64() : 0));
            HANDLE handles[3] = { stopEvent.get(), wakeEvent.get(), watch.get() };
            DWORD r = WaitForMultipleObjects(watch ? 3 : 2, handles, FALSE, timeout);
            if (r == WAIT_OBJECT_0) break;
            if (r == WAIT_OBJECT_0 + 2) { FindNextChangeNotification(watch.get()); Sleep(200); Reload(); }
            auto now = GetTickCount64();
            if (ULONGLONG at = pauseAt_; at && now >= at) { pauseAt_ = 0; optimizer_.Pause(); Broadcast(); }
            if (ULONGLONG at = agentCheckAt_; at && now >= at) { agentCheckAt_ = 0; EnsureAgent(); }
            if (detector_.Confirming()) detector_.Tick();
            if (optimizer_.WantsTick() && now - lastTick >= 2000) { optimizer_.Tick(); lastTick = now; }
            if (optimizer_.Engaged() && now - lastStatus >= 1000) { Broadcast(); lastStatus = now; }
        }
        Shutdown();
        return 0;
    }

    // A user signed in: check a little later that the agent came up with them.
    void SessionChanged(DWORD event, DWORD session) {
        if (event == WTS_SESSION_LOGON && session == dismissedSession_) dismissedSession_ = kNoSession;
        if (event == WTS_SESSION_LOGON || event == WTS_CONSOLE_CONNECT || event == WTS_SESSION_UNLOCK) { agentCheckAt_ = GetTickCount64() + kAgentGraceMs; Wake(); }
    }

    // Also called from the crash filter: must work from any thread, at any time.
    void Shutdown() {
        if (stopped_.exchange(true)) return;
        optimizer_.Disengage();
        server_.Stop();
        etw_.Stop();
        Log(L"Core", L"stopped");
    }

private:
    void Wire() {
        detector_.OnGameStarted = [this](const GameInfo& g) {
            if (config_.enabled && !manualPause_) optimizer_.Engage(g, Protected());
            if (config_.notifications) server_.Broadcast(ipc::Message("notify").Set("title", L"Game mode on").Set("text", g.name));
            Wake();
        };
        // Overlays, the EAC splash and notifications take focus for a moment; pausing on each one
        // would create and delete a power plan every few seconds (seen with Rust). Pause after 2 s away.
        detector_.OnGameFocus = [this](const GameInfo&, bool front) {
            if (front) { pauseAt_ = 0; optimizer_.Resume(); }
            else pauseAt_ = GetTickCount64() + 2000;
            Wake();
        };
        detector_.OnGameEnded = [this](const GameInfo&) { pauseAt_ = 0; optimizer_.Disengage(); Wake(); };
        detector_.OnVerdict = [this](const std::wstring& exe, const std::wstring& verdict, const std::wstring& reasons) {
            ipc::Message m("detected");
            m.Set("exe", exe).Set("name", FriendlyName(exe)).Set("verdict", verdict).Set("reasons", reasons);
            {
                std::lock_guard lock(historyGate_);
                if (!history_.empty() && history_.back().GetW("exe") == exe && history_.back().Get("verdict") == m.Get("verdict")) return;
                history_.push_back(m);
                if (history_.size() > 30) history_.pop_front();
            }
            server_.Broadcast(m);
        };
        etw_.OnProcessStart = [this](DWORD pid, const ProcessRecord& r) { optimizer_.OnProcessStart(pid, r.image); };
        etw_.OnProcessStop = [this](DWORD pid) { detector_.ProcessExited(pid); };
        optimizer_.OnMmcss = [this](bool on) { server_.Broadcast(ipc::Message("mmcss").Set("on", on)); };

        server_.OnConnect = [this](ipc::Server::ClientId id) {
            {
                std::lock_guard lock(agentsGate_);
                agents_.insert(id);
            }
            server_.Send(id, ConfigMessage());
            std::lock_guard lock(historyGate_);
            for (auto& m : history_) server_.Send(id, m);
            server_.Send(id, StatusMessage());
        };
        server_.OnDisconnect = [this](ipc::Server::ClientId id) {
            bool last;
            {
                std::lock_guard lock(agentsGate_);
                agents_.erase(id);
                agentPids_.erase(id);
                last = agents_.empty();
            }
            // without the agent there is no foreground information; play safe
            if (last) { optimizer_.Pause(); Wake(); }
        };
        server_.OnMessage = [this](ipc::Server::ClientId id, const ipc::Message& m) { OnMessage(id, m); };
    }

    void OnMessage(ipc::Server::ClientId id, const ipc::Message& m) {
        if (m.type == "hello") {
            std::lock_guard lock(agentsGate_);
            agentPids_[id] = static_cast<DWORD>(m.GetInt("pid"));
        } else if (m.type == "foreground") {
            auto facts = WindowFacts::FromMessage(m);
            optimizer_.OnForeground(facts.pid);
            detector_.Foreground(facts);
        } else if (m.type == "set") {
            std::lock_guard lock(configGate_);
            auto key = m.GetW("key");
            if (config_.Set(key, m.GetW("value"))) {
                if (key.starts_with(L"background.") && key.ends_with(L"_list")) config_.SaveBackground();
                else config_.Save();
                Apply();
            }
            server_.Broadcast(ConfigMessage());
        } else if (m.type == "pause") {
            manualPause_ = true;
            optimizer_.Disengage();
        } else if (m.type == "resume") {
            manualPause_ = false;
            if (auto g = detector_.Game(); g && detector_.GameInFront() && config_.enabled) optimizer_.Engage(*g, Protected());
        } else if (m.type == "lists") {
            auto split = [](const std::wstring& text) { std::vector<std::wstring> items; for (auto& s : Split(text, L'|')) if (!s.empty()) items.push_back(s); return items; };
            detector_.SetLists(split(m.GetW("forced")), split(m.GetW("blocked")));
        } else if (m.type == "bye") {
            dismissedSession_ = static_cast<DWORD>(m.GetInt("session"));
        } else if (m.type == "rescan") {
            detector_.Forget();
        } else if (m.type == "status") {
            server_.Send(id, StatusMessage());
        }
        Wake();
    }

    void Reload() {
        std::lock_guard lock(configGate_);
        config_ = Config::Load();
        Apply();
        server_.Broadcast(ConfigMessage());
    }

    void Apply() {
        optimizer_.SetConfig(config_);
        detector_.SetConfig(config_);
        if (!config_.enabled) optimizer_.Disengage();
        else if (auto g = detector_.Game(); g && detector_.GameInFront() && !optimizer_.Engaged() && !manualPause_) optimizer_.Engage(*g, Protected());
        SyncSignIn();
    }

    // The sign-in entry is for every user, so it lives in HKLM, which only the service may write.
    void SyncSignIn() {
        if (!asService) return;
        const std::wstring run = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run";
        if (config_.startWithWindows) RegWriteString(HKEY_LOCAL_MACHINE, run, L"xwwPulse", L"\"" + ModuleDir() + L"\\xwwPulse.exe\" --tray");
        else RegDeleteValueAt(HKEY_LOCAL_MACHINE, run, L"xwwPulse");
    }

    std::set<DWORD> Protected() {
        std::lock_guard lock(agentsGate_);
        std::set<DWORD> pids;
        for (auto& [id, pid] : agentPids_) pids.insert(pid);
        return pids;
    }

    ipc::Message ConfigMessage() {
        ipc::Message m("config");
        for (auto& [k, v] : config_.Flatten()) m.Set(k, v);
        m.Set("topology", TopologyText());
        return m;
    }

    ipc::Message StatusMessage() {
        ipc::Message m("status");
        auto game = detector_.Game();
        auto phase = detector_.Phase();
        if (!config_.enabled) phase = L"disabled";
        else if (manualPause_) phase = L"off";
        m.Set("phase", phase).Set("etw", etw_.Events() > 0).Set("version", std::wstring(XWW_VERSION));
        if (game) {
            m.Set("game", game->name).Set("exe", game->exe).Set("pid", static_cast<long long>(game->id.pid))
                .Set("fps", etw_.Fps(game->id.pid, 1.0)).Set("reasons", game->reasons).Set("anticheat", game->antiCheat).Set("forced", game->forced);
        }
        m.Set("steps", EncodeSteps(optimizer_.Steps()));
        return m;
    }

    // Without the agent there is no foreground information and detection stands still. It starts at sign-in from
    // the Run key, but that entry can be disabled in Task Manager or the app closed; start it in the console session then.
    void EnsureAgent() {
        if (!config_.startWithWindows) return;      // the user starts the app themselves
        {
            std::lock_guard lock(agentsGate_);
            if (!agents_.empty()) return;
        }
        DWORD session = WTSGetActiveConsoleSessionId();
        if (session == kNoSession || session == dismissedSession_) return;
        HANDLE raw = nullptr;
        if (!WTSQueryUserToken(session, &raw)) return;          // nobody signed in
        Handle token(raw);
        void* environment = nullptr;
        CreateEnvironmentBlock(&environment, token.get(), FALSE);
        auto exe = ModuleDir() + L"\\xwwPulse.exe";
        auto command = L"\"" + exe + L"\" --tray";
        STARTUPINFOW startup{ sizeof(startup) };
        startup.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
        PROCESS_INFORMATION process{};
        if (CreateProcessAsUserW(token.get(), exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, environment, ModuleDir().c_str(), &startup, &process)) {
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            Log(L"Core", L"started xwwPulse in session {}: it was not running", session);
        } else Log(L"Core", L"could not start xwwPulse in session {} ({})", session, GetLastError());
        if (environment) DestroyEnvironmentBlock(environment);
    }

    void Broadcast() { server_.Broadcast(StatusMessage()); }
    void Wake() { SetEvent(wakeEvent.get()); Broadcast(); }

    bool dryRun_;
    std::atomic<bool> stopped_{ false }, manualPause_{ false };
    static constexpr DWORD kNoSession = 0xFFFFFFFF;
    static constexpr ULONGLONG kAgentGraceMs = 20000;
    std::atomic<ULONGLONG> pauseAt_{ 0 };      // Alt+Tab debounce
    std::atomic<ULONGLONG> agentCheckAt_{ 0 };
    std::atomic<DWORD> dismissedSession_{ kNoSession };
    std::mutex configGate_, agentsGate_, historyGate_;
    Config config_;
    EtwTracker etw_;
    Detector detector_{ etw_ };
    Optimizer optimizer_;
    ipc::Server server_;
    std::set<ipc::Server::ClientId> agents_;
    std::map<ipc::Server::ClientId, DWORD> agentPids_;
    std::deque<ipc::Message> history_;
};

// Unhandled crash: put the system back before the process dies; the SCM then restarts us.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS*) {
    static std::atomic<bool> once{ false };
    if (!once.exchange(true) && running) {
        Log(L"Core", L"crashed; rolling back");
        __try { running->Shutdown(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ------------------------------------------------------------------ service plumbing

DWORD WINAPI ControlHandler(DWORD control, DWORD event, LPVOID data, LPVOID) {
    switch (control) {
    case SERVICE_CONTROL_SESSIONCHANGE:
        if (running && data) running->SessionChanged(event, static_cast<WTSSESSION_NOTIFICATION*>(data)->dwSessionId);
        return NO_ERROR;
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 10000);
        SetEvent(stopEvent.get());
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD, LPWSTR*) {
    statusHandle = RegisterServiceCtrlHandlerExW(kServiceName, ControlHandler, nullptr);
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 5000);
    ReportStatus(SERVICE_RUNNING);
    asService = true;
    Core core(false);
    int code = core.Run();
    ReportStatus(SERVICE_STOPPED, code);
}

BOOL WINAPI ConsoleHandler(DWORD) { SetEvent(stopEvent.get()); return TRUE; }

// ------------------------------------------------------------------ install

std::wstring Quoted(const std::wstring& s) { return L"\"" + s + L"\""; }

int Install() {
    auto exe = ModuleDir() + L"\\xwwPulseService.exe";
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE);
    if (!scm) { wprintf(L"Run this as administrator (error %lu).\n", GetLastError()); return 1; }
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, L"xwwPulse", SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
        Quoted(exe).c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE, Quoted(exe).c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
    if (!svc) { wprintf(L"CreateService failed (%lu).\n", GetLastError()); CloseServiceHandle(scm); return 1; }

    SERVICE_DESCRIPTIONW description{ const_cast<LPWSTR>(L"Detects games and tunes CPU, GPU, power and background apps while they run.") };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &description);
    SC_ACTION actions[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW failure{ 86400, nullptr, nullptr, 3, actions };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &failure);
    SERVICE_FAILURE_ACTIONS_FLAG flag{ TRUE };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);
    SERVICE_PRESHUTDOWN_INFO preshutdown{ 10000 };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &preshutdown);
    StartServiceW(svc, 0, nullptr);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);

    auto dashboard = ModuleDir() + L"\\xwwPulse.exe";
    RegWriteString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"xwwPulse", Quoted(dashboard) + L" --tray");
    Config::WriteDefaultsIfMissing();
    wprintf(L"xwwPulse service installed and started. xwwPulse Control starts at sign-in.\n");
    return 0;
}

int Uninstall() {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { wprintf(L"Run this as administrator (error %lu).\n", GetLastError()); return 1; }
    if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE)) {
        SERVICE_STATUS s{};
        ControlService(svc, SERVICE_CONTROL_STOP, &s);
        for (int i = 0; i < 50 && QueryServiceStatus(svc, &s) && s.dwCurrentState != SERVICE_STOPPED; ++i) Sleep(200);
        DeleteService(svc);
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    Optimizer::Recover();                       // anything a killed service left behind
    journal::Clear();
    RegDeleteValueAt(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", L"xwwPulse");
    wprintf(L"xwwPulse service removed. Settings in C:\\ProgramData\\xwwPulse are kept.\n");
    return 0;
}

int EtwTest(int seconds) {
    log::SetConsole(true);
    EtwTracker etw;
    etw.SetDiagnostics(true);
    if (!etw.Start()) { wprintf(L"ETW session failed; run as administrator.\n"); return 1; }
    etw.EnablePresent(true);
    wprintf(L"Frame presenters for %d s (read-only; nothing is changed). %s\n", seconds, TopologyText().c_str());
    for (int i = 0; i < seconds && WaitForSingleObject(stopEvent.get(), 1000) != WAIT_OBJECT_0; ++i) {
        std::wstring line = std::format(L"[{:>3}s] events {:>7} (DXGI {}, DxgKrnl {}) |", i + 1, etw.Events(), etw.DxgiEvents(), etw.KernelPresentEvents());
        for (auto& [pid, fps] : etw.TopFps(4)) {
            auto r = etw.Process(pid);
            line += std::format(L" {} ({}) {:.0f} fps |", r ? FileName(r->image) : L"?", pid, fps);
        }
        wprintf(L"%s\n", line.c_str());
    }
    std::wstring ids = L"DxgKrnl event IDs:";
    for (auto& [id, count] : etw.KernelEventIds()) ids += std::format(L" {}x{}", id, count);
    wprintf(L"%s\n", ids.c_str());
    for (auto& [pid, count] : etw.DxgiPids()) if (count > 50) { auto r = etw.Process(pid); wprintf(L"DXGI pid %lu %s: %llu\n", pid, r ? FileName(r->image).c_str() : L"?", count); }
    etw.Stop();
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    stopEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wakeEvent.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    log::Init(L"Pulse.log");
    SetUnhandledExceptionFilter(CrashFilter);
    std::wstring mode = argc > 1 ? Lower(argv[1]) : L"";

    if (mode == L"--install") return Install();
    if (mode == L"--uninstall") return Uninstall();
    if (mode == L"--recover") { log::SetConsole(true); Optimizer::Recover(); return 0; }
    if (mode == L"--etw-test") { SetConsoleCtrlHandler(ConsoleHandler, TRUE); return EtwTest(argc > 2 ? _wtoi(argv[2]) : 15); }
    if (mode == L"--console" || mode == L"--detect-only") {
        log::SetConsole(true);
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        Core core(mode == L"--detect-only");
        return core.Run();
    }
    if (!mode.empty()) {
        wprintf(L"xwwPulseService %s\n  --install | --uninstall | --console | --detect-only | --etw-test [seconds] | --recover\n", XWW_VERSION);
        return 2;
    }
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(kServiceName), ServiceMain }, { nullptr, nullptr } };
    if (!StartServiceCtrlDispatcherW(table)) {
        wprintf(L"xwwPulseService runs as a Windows service. Use --install, or --console to try it here.\n");
        return 1;
    }
    return 0;
}
