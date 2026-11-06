#include "agent/agent.h"

#include <dwmapi.h>

namespace xww {

bool Agent::Start(HWND window) {
    window_ = window;
    hook_.OnFacts = [this](const WindowFacts& f) { if (client_.Connected()) client_.Send(f.ToMessage()); };
    client_.OnMessage = [this](const ipc::Message& m) {
        if (m.type == "mmcss") Mmcss(m.GetBool("on"));      // straight away: DWM should not wait for the UI thread
        PostMessageW(window_, WM_PULSE, 0, reinterpret_cast<LPARAM>(new ipc::Message(m)));
    };
    client_.OnConnection = [this](bool connected) { PostMessageW(window_, WM_PULSE_CONNECTION, connected, 0); };
    client_.Start();
    return hook_.Start(window);
}

void Agent::Stop() {
    hook_.Stop();
    client_.Stop();
    Mmcss(false);
}

void Agent::Dismiss() {
    DWORD session = 0;
    ProcessIdToSessionId(GetCurrentProcessId(), &session);
    Send(ipc::Message("bye").Set("session", static_cast<long long>(session)));
}

void Agent::OnConnection(bool connected) {
    if (!connected) { Mmcss(false); hook_.SetConfirming(false); return; }
    Send(ipc::Message("hello").Set("pid", static_cast<long long>(GetCurrentProcessId())));
    SendLists();
    hook_.Probe();
}

void Agent::OnMessage(const ipc::Message& m) {
    if (m.type == "status") {
        auto phase = m.Get("phase");
        hook_.SetConfirming(phase == "confirming");
        hook_.SetSettled(phase == "game" || phase == "off" || phase == "disabled");
    }
}

// DWM schedules its threads under MMCSS for as long as this process asks for it (and is alive).
void Agent::Mmcss(bool on) {
    if (mmcss_ == on) return;
    mmcss_ = on;
    HRESULT hr = DwmEnableMMCSS(on);
    Log(L"Agent", L"DWM MMCSS {} (0x{:08X})", on ? L"on" : L"off", static_cast<uint32_t>(hr));
}

// ------------------------------------------------------------------ lists

std::vector<std::wstring> Agent::List(const wchar_t* name) {
    std::vector<std::wstring> items;
    auto key = std::wstring(kUserKey) + L"\\" + name;
    for (auto& v : RegValueNames(HKEY_CURRENT_USER, key)) items.push_back(v);
    return items;
}

void Agent::AddToList(const wchar_t* name, const std::wstring& exe) {
    auto other = std::wstring(name) == L"ForcedGames" ? L"BlockedApps" : L"ForcedGames";
    RegDeleteValueAt(HKEY_CURRENT_USER, std::wstring(kUserKey) + L"\\" + other, exe.c_str());
    RegWriteString(HKEY_CURRENT_USER, std::wstring(kUserKey) + L"\\" + name, exe.c_str(), FileName(exe));
    Changed();
}

void Agent::RemoveFromList(const wchar_t* name, const std::wstring& exe) {
    RegDeleteValueAt(HKEY_CURRENT_USER, std::wstring(kUserKey) + L"\\" + name, exe.c_str());
    Changed();
}

void Agent::SendLists() {
    auto join = [](const wchar_t* name) {
        std::wstring out;
        for (auto& exe : List(name)) out += (out.empty() ? L"" : L"|") + exe;     // "|" cannot occur in a path
        return out;
    };
    Send(ipc::Message("lists").Set("forced", join(L"ForcedGames")).Set("blocked", join(L"BlockedApps")));
}

void Agent::Changed() {
    SendLists();                        // the service drops its cached verdicts with the new lists
    hook_.Probe();
}

}  // namespace xww
