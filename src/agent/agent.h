//  The service's eyes in the user's session, hosted by xwwPulse.exe:
//  the foreground hook (window facts), the pipe client, the ForcedGames /
//  BlockedApps lists in HKCU (sent to the service, which matches them
//  against the exe it resolved itself) and DwmEnableMMCSS, which only
//  works from a process in this session. Pipe messages are handed to the
//  window thread (WM_PULSE) so all state is touched on one thread.
#pragma once

#include "detection/foreground_hook.h"

namespace xww {

inline constexpr UINT WM_PULSE = WM_APP + 1;                // lParam: ipc::Message* (receiver deletes)
inline constexpr UINT WM_PULSE_CONNECTION = WM_APP + 2;     // wParam: connected

class Agent {
public:
    bool Start(HWND window);
    void Stop();
    void Dismiss();                                         // the user chose Exit: the service does not start the agent again in this session
    void OnTimer(UINT_PTR id) { hook_.OnTimer(id); }
    void OnMessage(const ipc::Message& m);                  // on the window thread, before the UI sees it
    void OnConnection(bool connected);
    bool Send(const ipc::Message& m) { return client_.Send(m); }
    bool Connected() const { return client_.Connected(); }

    void Set(const std::string& key, const std::string& value) { Send(ipc::Message("set").Set("key", key).Set("value", value)); }

    static std::vector<std::wstring> List(const wchar_t* name);          // L"ForcedGames" | L"BlockedApps"
    void AddToList(const wchar_t* name, const std::wstring& exe);       // and removes it from the other list
    void RemoveFromList(const wchar_t* name, const std::wstring& exe);

private:
    void Changed();
    void SendLists();
    void Mmcss(bool on);

    ForegroundHook hook_;
    ipc::Client client_;
    HWND window_ = nullptr;
    bool mmcss_ = false;
};

}  // namespace xww
