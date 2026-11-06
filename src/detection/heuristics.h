//  Level 1 static heuristics. Everything here works from the exe on disk,
//  the folder around it and the module names ETW reported, never from a
//  handle to the running game, so anti-cheats have nothing to object to.
#pragma once

#include "config/config.h"
#include "detection/facts.h"

namespace xww {

// Module markers gathered from PE imports and ETW image loads.
enum ModuleFlags : uint32_t {
    kModule3D = 1 << 0,             // d3d9/10/11/12, dxgi, vulkan-1, opengl32
    kModuleInput = 1 << 1,          // xinput*, gameinput, dinput8, Windows.Gaming.Input
    kModuleStop = 1 << 2,           // libcef, node, chrome_elf, Qt WebEngine, WPF, WinForms
    kModuleEngine = 1 << 3,         // UnityPlayer, GameAssembly, DLSS/FSR/XeSS, Steamworks, EOS, Bink, FMOD...
};

uint32_t ClassifyModule(std::wstring_view moduleName);          // lower-case file name -> flags

struct StaticFacts {
    bool nonGame = false;           // built-in or configured non_games list
    bool inGameDir = false;         // a configured game folder pattern
    std::wstring store;             // the launcher whose install folder holds the exe, from GameLibrary
    bool launcherChild = false;
    uint32_t modules = 0;           // ModuleFlags from imports, folder fingerprints and ETW loads
    std::wstring engine;            // "Unity", "Unreal Engine", "Steamworks"... for the dashboard
};

// imports of the exe on disk (including delay-load), plus engine fingerprints next to it
uint32_t ScanExecutable(const std::wstring& exePath, std::wstring* engine);

StaticFacts Analyze(const std::wstring& exePath, const std::wstring& parentExe, uint32_t loadedModules, const std::wstring& store, const Config& config);

struct Score {
    int points = 0;
    bool rejected = false;
    std::wstring reasons;           // "game folder, borderless fullscreen, Unity" for the log and dashboard
};

Score Evaluate(const StaticFacts& s, const WindowFacts& w);

std::wstring DetectAntiCheat(const std::wstring& exePath);       // running anti-cheat service or folder marker, or empty

}  // namespace xww
