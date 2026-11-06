//  xwwPulse.exe, the app and the agent, in the user's session.
//
//    xwwPulse.exe                  open the window (or bring the running one to the front)
//    xwwPulse.exe --tray           start in the notification area (at sign-in)
//    xwwPulse.exe --render <dir>   save every page as PNG, dark and light, with sample data
#include "config/config.h"
#include "ui/tray.h"
#include "ui/window.h"

#include <objbase.h>
#include <shellapi.h>

using namespace xww;
using namespace xww::ui;

namespace {

// Sample session for --render, so every page shows its full layout.
void FillSample(Model& m) {
    m.connected = true;
    m.phase = L"game";
    m.game = L"Example Game";
    m.exe = L"D:\\SteamLibrary\\steamapps\\common\\Example Game\\ExampleGame.exe";
    m.pid = 4242;
    m.fps = 144;
    m.reasons = L"Steam library, started by a game launcher, Unreal Engine, borderless fullscreen, cursor locked to the window, 3D API, 143 fps";
    m.antiCheat = L"Easy Anti-Cheat";
    m.topology = L"2 L3 domains: 96 MB (CPU 0-15), 32 MB (CPU 16-31)";
    for (int i = 0; i < 60; ++i) m.fpsSeries.push_back(static_cast<float>(140 + (i * 37 % 11) - (i % 7 == 0 ? 25 : 0)));
    m.steps = {
        { L"cstates", L"Processor idle states", StepState::On, L"cores stay out of deep C-states" },
        { L"priority", L"CPU priority", StepState::On, L"High (was Normal)" },
        { L"gpu", L"GPU scheduling priority", StepState::On, L"High" },
        { L"ccd", L"CPU placement", StepState::On, L"CCD0 · 96 MB L3 (V-Cache) · CPU Sets" },
        { L"threads", L"Game thread boost", StepState::Skipped, L"Easy Anti-Cheat is running; thread access stays off" },
        { L"suspend", L"Suspend background apps", StepState::On, L"searchindexer.exe, onedrive.exe" },
        { L"throttle", L"Throttle background apps", StepState::On, L"14 processes on EcoQoS and very low I/O" },
        { L"mmcss", L"DWM multimedia scheduling", StepState::On, L"compositor threads under MMCSS" },
    };
    auto d = Config::Defaults();
    for (auto& [k, v] : d.Flatten()) m.config[k] = v;
    m.config["background.suspend_list"] = R"(["searchindexer.exe", "onedrive.exe", "teams.exe", "ms-teams.exe", "obs64.exe"])";   // one default removed, one app added
    m.history = {
        { m.exe, L"Example Game", L"game", m.reasons },
        { L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", L"Google Chrome", L"not a game", L"on the non-games list" },
        { L"C:\\Tools\\Benchmark\\bench.exe", L"Benchmark", L"undecided", L"2 of 3 points (borderless fullscreen, 3D API)" },
    };
}

int Render(const std::wstring& folder) {
    CreateDirectoryW(folder.c_str(), nullptr);
    Model model;
    FillSample(model);
    Window window(nullptr, model);
    const std::pair<Page, const wchar_t*> pages[] = {
        { Page::Overview, L"overview" }, { Page::Optimizations, L"optimizations" }, { Page::Games, L"games" },
        { Page::Background, L"background" }, { Page::Logs, L"logs" }, { Page::Settings, L"settings" },
    };
    int failed = 0;
    for (auto& [page, name] : pages) {
        failed += !window.RenderToFile(page, folder + L"\\" + name + L".png");
    }
    model = Model{};                                   // and the first-run state: no service yet
    failed += !window.RenderToFile(Page::Overview, folder + L"\\no-service.png");
    window.Pick(L"Choose a game to always optimize", nullptr);     // lists the apps running on this machine
    failed += !window.RenderToFile(Page::Games, folder + L"\\picker.png");
    return failed;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring mode = argc > 1 ? Lower(argv[1]) : L"";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    log::Init(L"PulseDashboard.log");

    if (mode == L"--render" && argc > 2) return Render(argv[2]);

    // one dashboard per user session; a second start brings the first to the front
    Handle single(CreateMutexW(nullptr, TRUE, L"Local\\xwwPulseDashboard"));
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(L"xwwPulseDashboard", nullptr)) {
            ShowWindow(other, SW_SHOW);
            ShowWindow(other, SW_RESTORE);
            SetForegroundWindow(other);
        }
        return 0;
    }

    bool tray = mode == L"--tray";
    Model model;
    Agent agent;
    Window window(&agent, model);
    Tray icon;
    if (!window.Create(!tray)) return 1;
    window.SetHideOnClose(true);
    icon.Create(instance);
    icon.OnOpen = [&] { window.Show(); };
    icon.OnExit = [&] { agent.Dismiss(); window.SetHideOnClose(false); PostMessageW(window.Hwnd(), WM_CLOSE, 0, 0); };
    icon.OnPause = [&] { agent.Send(ipc::Message("pause")); };
    icon.OnResume = [&] { agent.Send(ipc::Message("resume")); };
    window.OnStateChanged = [&] { icon.Update(model); };
    window.OnNotify = [&](const ipc::Message& m) { icon.Notify(m.GetW("title"), m.GetW("text")); };

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    icon.Destroy();
    CoUninitialize();
    return 0;
}
