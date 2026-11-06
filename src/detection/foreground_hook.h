//  Agent side of detection, in the user's session (part of xwwPulse.exe).
//  SetWinEventHook(WINEVENT_OUTOFCONTEXT) delivers foreground and minimize
//  events to this thread's message loop; nothing is loaded into any other
//  process. Games often show a launcher or a windowed splash first and go
//  fullscreen seconds later, so each change is probed again with growing
//  gaps for a minute. While the service waits for frames, the cursor state
//  is sampled every 500 ms for up to 4 s.
#pragma once

#include "detection/facts.h"

#include <unordered_set>

namespace xww {

class ForegroundHook {
public:
    std::function<void(const WindowFacts&)> OnFacts;

    bool Start(HWND messageWindow);             // timers are posted to this window
    void Stop();
    void OnTimer(UINT_PTR id);                  // forward WM_TIMER for ids >= kTimerBase
    void Probe();                               // probe the current foreground window now
    void SetConfirming(bool on);                // the service waits for frames: keep sampling the cursor
    void SetSettled(bool on);                   // the foreground is a confirmed game: no more re-probes

    static constexpr UINT_PTR kTimerBase = 0x5000;
    WindowFacts Inspect(HWND hwnd);             // window, cursor and fullscreen facts

private:
    static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD);
    void ScheduleProbe();
    bool InWindowsGameList(const std::wstring& exe);

    HWINEVENTHOOK hooks_[2] = {};
    HWND window_ = nullptr;
    WindowFacts last_;
    int samples_ = 0;
    size_t step_ = 0;
    bool confirming_ = false, settled_ = false;
    std::unordered_set<std::wstring> windowsGames_;
    ULONGLONG windowsGamesRead_ = 0;
};

}  // namespace xww
