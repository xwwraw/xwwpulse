//  What the agent learns about the foreground window in the user's session.
//  The service cannot see the desktop from session 0, so these arrive over
//  the pipe.
#pragma once

#include "core/ipc.h"

namespace xww {

struct WindowFacts {
    DWORD pid = 0;
    std::wstring exe;               // full path, or empty when the agent may not query the process
    std::wstring title;
    bool borderless = false;        // WS_POPUP without WS_CAPTION / WS_THICKFRAME
    bool coversMonitor = false;     // window rect == monitor rect (not just the work area)
    bool cursorHidden = false;      // GetCursorInfo: not showing, suppressed or no cursor shape
    bool cursorClipped = false;     // GetClipCursor inside the window
    bool d3dFullscreen = false;     // SHQueryUserNotificationState == QUNS_RUNNING_D3D_FULL_SCREEN
    bool minimized = false;
    bool windowsGame = false;       // listed in the user's Windows game list (HKCU\System\GameConfigStore)

    ipc::Message ToMessage() const {
        return ipc::Message("foreground").Set("pid", static_cast<long long>(pid)).Set("exe", exe).Set("title", title)
            .Set("borderless", borderless).Set("covers", coversMonitor).Set("cursor_hidden", cursorHidden)
            .Set("cursor_clipped", cursorClipped).Set("d3d_fullscreen", d3dFullscreen).Set("minimized", minimized)
            .Set("windows_game", windowsGame);
    }

    static WindowFacts FromMessage(const ipc::Message& m) {
        WindowFacts f;
        f.pid = static_cast<DWORD>(m.GetInt("pid"));
        f.exe = m.GetW("exe");
        f.title = m.GetW("title");
        f.borderless = m.GetBool("borderless");
        f.coversMonitor = m.GetBool("covers");
        f.cursorHidden = m.GetBool("cursor_hidden");
        f.cursorClipped = m.GetBool("cursor_clipped");
        f.d3dFullscreen = m.GetBool("d3d_fullscreen");
        f.minimized = m.GetBool("minimized");
        f.windowsGame = m.GetBool("windows_game");
        return f;
    }
};

}  // namespace xww
