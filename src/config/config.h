//  Profiles: config.toml (behaviour) and background.toml (process lists),
//  both in C:\ProgramData\xwwPulse. Missing files or keys fall back to
//  the defaults below; the service writes config.toml back when the
//  dashboard changes a setting.
#pragma once

#include "core/common.h"

#include <map>

namespace xww {

struct Config {
    // [general]
    bool enabled = true;
    bool pauseOnAltTab = true;
    bool notifications = true;
    bool startWithWindows = true;                  // the app starts at sign-in, and the service starts it when it is missing

    // [detection]
    double minFps = 20;
    double confirmSeconds = 2;
    int scoreThreshold = 3;
    std::vector<std::wstring> gameDirs, launchers, nonGames;

    // [power]
    bool idleDisable = true;
    bool skipOnBattery = true;

    // [gpu]
    std::wstring processPriority = L"high";        // normal | above_normal | high
    std::wstring gpuPriority = L"high";            // off | high | realtime
    bool dwmMmcss = true;

    // [cpu]
    std::wstring ccd = L"auto";                    // auto | ccd0 | vcache | off
    std::wstring cpuSetMode = L"soft";             // soft (CPU Sets) | hard (affinity mask)
    bool backgroundCores = true;                   // listed background apps on the cores the game does not use
    bool threadBoost = false;
    bool threadBoostWithAntiCheat = false;

    // [background] + background.toml
    bool suspendBackground = true;
    bool throttleBackground = true;
    std::vector<std::wstring> suspendList, throttleList, coreList;

    static Config Defaults();
    static Config Load();                          // defaults, then config.toml and background.toml on top
    bool Save() const;                             // config.toml
    bool SaveBackground() const;                   // background.toml
    bool Set(const std::wstring& key, const std::wstring& value);   // "gpu.priority", "high"; false when unknown or invalid
    std::map<std::string, std::string> Flatten() const;            // key -> text, for the dashboard

    static std::wstring ConfigPath();
    static std::wstring BackgroundPath();
    static void WriteDefaultsIfMissing();
};

}  // namespace xww
