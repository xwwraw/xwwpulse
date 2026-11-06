#include "config/config.h"
#include "config/toml.h"

#include <algorithm>

namespace xww {

std::wstring Config::ConfigPath() { return ProgramDataDir() + L"\\config.toml"; }
std::wstring Config::BackgroundPath() { return ProgramDataDir() + L"\\background.toml"; }

Config Config::Defaults() {
    Config c;
    c.gameDirs = {
        L"\\steamapps\\common\\", L"\\Epic Games\\", L"\\Riot Games\\", L"\\GOG Games\\", L"\\GOG Galaxy\\Games\\",
        L"\\Ubisoft Game Launcher\\games\\", L"\\EA Games\\", L"\\Origin Games\\", L"\\Rockstar Games\\", L"\\XboxGames\\",
        L"\\Battle.net\\", L"\\Games\\",
    };
    c.launchers = {
        L"steam.exe", L"epicgameslauncher.exe", L"battle.net.exe", L"eadesktop.exe", L"upc.exe", L"ubisoftconnect.exe",
        L"galaxyclient.exe", L"riotclientservices.exe", L"rockstarservice.exe", L"launcher.exe", L"playnite.desktopapp.exe",
        L"playnite.fullscreenapp.exe", L"xboxpcapp.exe",
    };
    c.nonGames = {
        // browsers and Electron/CEF apps: fullscreen video presents at 60 Hz too
        L"chrome.exe", L"msedge.exe", L"brave.exe", L"firefox.exe", L"opera.exe", L"vivaldi.exe", L"librewolf.exe", L"thorium.exe",
        L"discord.exe", L"vesktop.exe", L"steam.exe", L"steamwebhelper.exe", L"epicgameslauncher.exe", L"code.exe", L"spotify.exe",
        L"teams.exe", L"ms-teams.exe", L"slack.exe", L"telegram.exe", L"whatsapp.exe", L"msedgewebview2.exe",
        // media players
        L"vlc.exe", L"mpv.exe", L"mpvnet.exe", L"mpc-hc64.exe", L"mpc-be64.exe", L"potplayermini64.exe", L"wmplayer.exe",
        L"video.ui.exe", L"microsoft.media.player.exe", L"obs64.exe",
        // office and shell
        L"winword.exe", L"excel.exe", L"powerpnt.exe", L"outlook.exe", L"olk.exe", L"onenote.exe", L"explorer.exe",
        L"applicationframehost.exe", L"devenv.exe", L"xwwpulse.exe",
        // anti-cheat bootstrappers sit in the game folder next to the engine files
        L"start_protected_game.exe", L"easyanticheat_eos_setup.exe", L"easyanticheat_setup.exe", L"easyanticheat.exe",
        L"easyanticheat_eos.exe", L"beservice.exe", L"beservice_x64.exe", L"battleye launcher.exe",
    };
    c.suspendList = { L"searchindexer.exe", L"onedrive.exe", L"teams.exe", L"ms-teams.exe", L"dropbox.exe" };
    c.throttleList = { L"chrome.exe", L"brave.exe", L"msedge.exe", L"firefox.exe", L"discord.exe", L"steamwebhelper.exe", L"epicwebhelper.exe" };
    c.coreList = {
        L"discord.exe", L"vesktop.exe", L"chrome.exe", L"msedge.exe", L"brave.exe", L"firefox.exe", L"opera.exe", L"vivaldi.exe",
        L"spotify.exe", L"obs64.exe", L"steamwebhelper.exe", L"epicwebhelper.exe", L"telegram.exe", L"wallpaper64.exe",
    };
    return c;
}

static std::vector<std::wstring> LowerAll(std::vector<std::wstring> items) {
    for (auto& s : items) s = Lower(Trim(s));
    return items;
}

static bool OneOf(const std::wstring& v, std::initializer_list<const wchar_t*> allowed) {
    for (auto a : allowed) if (v == a) return true;
    return false;
}

Config Config::Load() {
    Config c = Defaults();
    auto doc = toml::ParseFile(ConfigPath());
    for (auto& e : doc.errors) Log(L"Config", L"config.toml {}", e);

    c.enabled = doc.Bool(L"general.enabled", c.enabled);
    c.pauseOnAltTab = doc.Bool(L"general.pause_on_alt_tab", c.pauseOnAltTab);
    c.notifications = doc.Bool(L"general.notifications", c.notifications);
    c.startWithWindows = doc.Bool(L"general.start_with_windows", c.startWithWindows);

    c.minFps = std::clamp(doc.Double(L"detection.min_fps", c.minFps), 10.0, 240.0);
    c.confirmSeconds = std::clamp(doc.Double(L"detection.confirm_seconds", c.confirmSeconds), 0.5, 10.0);
    c.scoreThreshold = static_cast<int>(std::clamp<long long>(doc.Int(L"detection.score_threshold", c.scoreThreshold), 1, 10));
    // user lists extend the built-in ones
    for (auto& d : doc.List(L"detection.game_dirs", {})) c.gameDirs.push_back(d);
    for (auto& d : doc.List(L"detection.launchers", {})) c.launchers.push_back(Lower(d));
    for (auto& d : doc.List(L"detection.non_games", {})) c.nonGames.push_back(Lower(d));

    c.idleDisable = doc.Bool(L"power.idle_disable", c.idleDisable);
    c.skipOnBattery = doc.Bool(L"power.skip_on_battery", c.skipOnBattery);

    auto prio = Lower(doc.String(L"gpu.process_priority", c.processPriority));
    if (OneOf(prio, { L"normal", L"above_normal", L"high" })) c.processPriority = prio;
    auto gpu = Lower(doc.String(L"gpu.priority", c.gpuPriority));
    if (OneOf(gpu, { L"off", L"high", L"realtime" })) c.gpuPriority = gpu;
    c.dwmMmcss = doc.Bool(L"gpu.dwm_mmcss", c.dwmMmcss);

    auto ccd = Lower(doc.String(L"cpu.ccd", c.ccd));
    if (OneOf(ccd, { L"auto", L"ccd0", L"vcache", L"off" })) c.ccd = ccd;
    auto mode = Lower(doc.String(L"cpu.cpu_set_mode", c.cpuSetMode));
    if (OneOf(mode, { L"soft", L"hard" })) c.cpuSetMode = mode;
    c.backgroundCores = doc.Bool(L"cpu.background_cores", c.backgroundCores);
    c.threadBoost = doc.Bool(L"cpu.thread_boost", c.threadBoost);
    c.threadBoostWithAntiCheat = doc.Bool(L"cpu.thread_boost_with_anticheat", c.threadBoostWithAntiCheat);

    c.suspendBackground = doc.Bool(L"background.suspend", c.suspendBackground);
    c.throttleBackground = doc.Bool(L"background.throttle", c.throttleBackground);

    auto bg = toml::ParseFile(BackgroundPath());
    for (auto& e : bg.errors) Log(L"Config", L"background.toml {}", e);
    c.suspendList = LowerAll(bg.List(L"suspend.processes", c.suspendList));
    c.throttleList = LowerAll(bg.List(L"throttle.processes", c.throttleList));
    c.coreList = LowerAll(bg.List(L"cores.processes", c.coreList));
    return c;
}

static std::wstring List(const std::vector<std::wstring>& items) {
    std::wstring out = L"[";
    for (size_t i = 0; i < items.size(); ++i) out += (i ? L", " : L"") + toml::Quote(items[i]);
    return out + L"]";
}

static bool WriteText(const std::wstring& path, const std::wstring& text) {
    auto temp = path + L".tmp";
    {
        Handle file(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!file) return false;
        auto utf8 = Narrow(text);
        DWORD written = 0;
        if (!WriteFile(file.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr)) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool Config::Save() const {
    auto d = Defaults();
    // only the entries the user added go back into the file; the built-in lists stay in code
    auto extra = [](const std::vector<std::wstring>& all, const std::vector<std::wstring>& builtIn) {
        std::vector<std::wstring> out;
        for (size_t i = builtIn.size(); i < all.size(); ++i) out.push_back(all[i]);
        return out;
    };
    auto b = [](bool v) { return v ? L"true" : L"false"; };
    auto text = std::format(
        L"#  How xwwPulse game mode behaves. xwwPulse rewrites this file when you\n"
        L"#  change a setting in the app; comments you add are not kept.\n"
        L"\n"
        L"[general]\n"
        L"enabled = {}\n"
        L"pause_on_alt_tab = {}          # undo power and background changes while the game is not in front\n"
        L"notifications = {}\n"
        L"start_with_windows = {}        # start the app at sign-in; games are detected only while it runs\n\n"
        L"[detection]\n"
        L"min_fps = {}                   # average frames per second that confirm a game\n"
        L"confirm_seconds = {}           # how long the average is measured\n"
        L"score_threshold = {}           # heuristic points a candidate needs (see docs/architecture.md)\n"
        L"game_dirs = {}                 # added to the built-in list\n"
        L"launchers = {}\n"
        L"non_games = {}\n\n"
        L"[power]\n"
        L"idle_disable = {}              # keep cores out of deep C-states while a game runs\n"
        L"skip_on_battery = {}\n\n"
        L"[gpu]\n"
        L"process_priority = {}          # normal | above_normal | high\n"
        L"priority = {}                  # GPU scheduling: off | high | realtime (realtime can starve the desktop)\n"
        L"dwm_mmcss = {}\n\n"
        L"[cpu]\n"
        L"ccd = {}                       # auto (V-Cache CCD on dual-CCD Ryzen, P-cores on Intel hybrid) | ccd0 | vcache | off\n"
        L"cpu_set_mode = {}              # soft: CPU Sets the scheduler may override | hard: affinity mask\n"
        L"background_cores = {}          # the [cores] list in background.toml goes to the cores the game does not use\n"
        L"thread_boost = {}              # hot game threads: no power throttling, own physical core\n"
        L"thread_boost_with_anticheat = {}\n\n"
        L"[background]\n"
        L"suspend = {}                   # freeze the [suspend] list in background.toml\n"
        L"throttle = {}                  # EcoQoS + very low I/O for the [throttle] list\n",
        b(enabled), b(pauseOnAltTab), b(notifications), b(startWithWindows),
        minFps, confirmSeconds, scoreThreshold, List(extra(gameDirs, d.gameDirs)), List(extra(launchers, d.launchers)), List(extra(nonGames, d.nonGames)),
        b(idleDisable), b(skipOnBattery),
        toml::Quote(processPriority), toml::Quote(gpuPriority), b(dwmMmcss),
        toml::Quote(ccd), toml::Quote(cpuSetMode), b(backgroundCores), b(threadBoost), b(threadBoostWithAntiCheat),
        b(suspendBackground), b(throttleBackground));
    return WriteText(ConfigPath(), text);
}

void Config::WriteDefaultsIfMissing() {
    if (GetFileAttributesW(ConfigPath().c_str()) == INVALID_FILE_ATTRIBUTES) Defaults().Save();
    if (GetFileAttributesW(BackgroundPath().c_str()) == INVALID_FILE_ATTRIBUTES) Defaults().SaveBackground();
}

bool Config::SaveBackground() const {
    return WriteText(BackgroundPath(), std::format(
        L"#  Processes limited while a game runs. Names are exe file names.\n"
        L"#  [suspend]  frozen completely (NtSuspendProcess), resumed afterwards\n"
        L"#  [throttle] EcoQoS, very low I/O priority and no dynamic boost\n"
        L"#  [cores]    moved to the cores the game does not use (other CCD or E-cores)\n"
        L"#  A process you bring to the front is released at once.\n"
        L"\n"
        L"[suspend]\nprocesses = {}\n\n[throttle]\nprocesses = {}\n\n[cores]\nprocesses = {}\n",
        List(suspendList), List(throttleList), List(coreList)));
}

bool Config::Set(const std::wstring& key, const std::wstring& raw) {
    auto value = Lower(Trim(raw));
    bool on = value == L"true" || value == L"1";
    bool isBool = on || value == L"false" || value == L"0";
    auto flag = [&](bool& target) { if (!isBool) return false; target = on; return true; };
    if (key == L"general.enabled") return flag(enabled);
    if (key == L"general.pause_on_alt_tab") return flag(pauseOnAltTab);
    if (key == L"general.notifications") return flag(notifications);
    if (key == L"general.start_with_windows") return flag(startWithWindows);
    // background lists arrive as exe file names joined by "|"
    auto list = [&](std::vector<std::wstring>& target) {
        std::vector<std::wstring> names;
        for (auto& item : Split(value, L'|')) {
            auto name = Trim(item);
            if (name.empty()) continue;
            if (!EndsWith(name, L".exe") || name.find_first_of(L"\\/:*?\"<>") != std::wstring::npos) return false;
            if (std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
        }
        target = std::move(names);
        return true;
    };
    if (key == L"background.suspend_list") return list(suspendList);
    if (key == L"background.throttle_list") return list(throttleList);
    if (key == L"background.cores_list") return list(coreList);
    if (key == L"power.idle_disable") return flag(idleDisable);
    if (key == L"power.skip_on_battery") return flag(skipOnBattery);
    if (key == L"gpu.dwm_mmcss") return flag(dwmMmcss);
    if (key == L"cpu.background_cores") return flag(backgroundCores);
    if (key == L"cpu.thread_boost") return flag(threadBoost);
    if (key == L"cpu.thread_boost_with_anticheat") return flag(threadBoostWithAntiCheat);
    if (key == L"background.suspend") return flag(suspendBackground);
    if (key == L"background.throttle") return flag(throttleBackground);
    if (key == L"gpu.process_priority" && OneOf(value, { L"normal", L"above_normal", L"high" })) { processPriority = value; return true; }
    if (key == L"gpu.priority" && OneOf(value, { L"off", L"high", L"realtime" })) { gpuPriority = value; return true; }
    if (key == L"cpu.ccd" && OneOf(value, { L"auto", L"ccd0", L"vcache", L"off" })) { ccd = value; return true; }
    if (key == L"cpu.cpu_set_mode" && OneOf(value, { L"soft", L"hard" })) { cpuSetMode = value; return true; }
    if (key == L"detection.min_fps") { double v = _wtof(value.c_str()); if (v < 10 || v > 240) return false; minFps = v; return true; }
    return false;
}

std::map<std::string, std::string> Config::Flatten() const {
    auto b = [](bool v) { return std::string(v ? "true" : "false"); };
    return {
        { "general.enabled", b(enabled) }, { "general.pause_on_alt_tab", b(pauseOnAltTab) }, { "general.notifications", b(notifications) },
        { "general.start_with_windows", b(startWithWindows) },
        { "detection.min_fps", std::format("{}", minFps) },
        { "power.idle_disable", b(idleDisable) }, { "power.skip_on_battery", b(skipOnBattery) },
        { "gpu.process_priority", Narrow(processPriority) }, { "gpu.priority", Narrow(gpuPriority) }, { "gpu.dwm_mmcss", b(dwmMmcss) },
        { "cpu.ccd", Narrow(ccd) }, { "cpu.cpu_set_mode", Narrow(cpuSetMode) }, { "cpu.background_cores", b(backgroundCores) }, { "cpu.thread_boost", b(threadBoost) },
        { "cpu.thread_boost_with_anticheat", b(threadBoostWithAntiCheat) },
        { "background.suspend", b(suspendBackground) }, { "background.throttle", b(throttleBackground) },
        { "background.suspend_list", Narrow(List(suspendList)) }, { "background.throttle_list", Narrow(List(throttleList)) },
        { "background.cores_list", Narrow(List(coreList)) },
    };
}

}  // namespace xww
