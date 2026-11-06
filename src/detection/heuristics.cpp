#include "detection/heuristics.h"

#include <array>
#include <map>
#include <mutex>
#include <shlwapi.h>

namespace xww {

// ------------------------------------------------------------------ module markers

uint32_t ClassifyModule(std::wstring_view raw) {
    auto name = FileName(raw);
    static const std::array<const wchar_t*, 8> graphics = { L"d3d9.dll", L"d3d10.dll", L"d3d10_1.dll", L"d3d11.dll", L"d3d12.dll", L"dxgi.dll", L"vulkan-1.dll", L"opengl32.dll" };
    static const std::array<const wchar_t*, 6> input = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll", L"gameinput.dll", L"dinput8.dll", L"windows.gaming.input.dll" };
    static const std::array<const wchar_t*, 9> stop = {
        L"libcef.dll", L"node.dll", L"chrome_elf.dll", L"qt6webenginecore.dll", L"qt5webenginecore.dll",
        L"presentationframework.dll", L"presentationframework.ni.dll", L"system.windows.forms.dll", L"system.windows.forms.ni.dll",
    };
    static const std::array<const wchar_t*, 16> engine = {
        L"unityplayer.dll", L"gameassembly.dll", L"nvngx_dlss.dll", L"sl.interposer.dll", L"libxess.dll", L"amd_fidelityfx_dx12.dll",
        L"amd_ags_x64.dll", L"steam_api64.dll", L"steam_api.dll", L"eossdk-win64-shipping.dll", L"galaxy64.dll", L"bink2w64.dll",
        L"fmod.dll", L"fmodstudio.dll", L"physx3_x64.dll", L"physxcommon_64.dll",
    };
    uint32_t flags = 0;
    for (auto m : graphics) if (name == m) flags |= kModule3D;
    for (auto m : input) if (name == m) flags |= kModuleInput;
    for (auto m : stop) if (name == m) flags |= kModuleStop;
    for (auto m : engine) if (name == m) flags |= kModuleEngine;
    if (name.starts_with(L"electron")) flags |= kModuleStop;
    return flags;
}

// ------------------------------------------------------------------ PE imports

namespace {

class MappedFile {
public:
    explicit MappedFile(const std::wstring& path) {
        file_.reset(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
        if (!file_) return;
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file_.get(), &size) || size.QuadPart < 1024 || size.QuadPart > (1LL << 31)) return;
        size_ = static_cast<size_t>(size.QuadPart);
        mapping_.reset(CreateFileMappingW(file_.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
        if (mapping_) view_ = static_cast<const uint8_t*>(MapViewOfFile(mapping_.get(), FILE_MAP_READ, 0, 0, 0));
    }
    ~MappedFile() { if (view_) UnmapViewOfFile(view_); }
    const uint8_t* data() const { return view_; }
    size_t size() const { return view_ ? size_ : 0; }
private:
    Handle file_, mapping_;
    const uint8_t* view_ = nullptr;
    size_t size_ = 0;
};

// Every read is bounds-checked: the file is untrusted input.
struct Pe {
    const uint8_t* base;
    size_t size;
    const IMAGE_SECTION_HEADER* sections = nullptr;
    WORD sectionCount = 0;

    template <class T> const T* At(size_t offset) const { return offset + sizeof(T) <= size ? reinterpret_cast<const T*>(base + offset) : nullptr; }

    std::optional<size_t> Offset(DWORD rva) const {
        for (WORD i = 0; i < sectionCount; ++i) {
            auto& s = sections[i];
            DWORD span = std::max(s.Misc.VirtualSize, s.SizeOfRawData);
            if (rva >= s.VirtualAddress && rva < s.VirtualAddress + span) {
                size_t off = size_t(rva - s.VirtualAddress) + s.PointerToRawData;
                return off < size ? std::optional<size_t>(off) : std::nullopt;
            }
        }
        return std::nullopt;
    }

    std::wstring Name(DWORD rva) const {
        auto off = Offset(rva);
        if (!off) return {};
        std::string s;
        for (size_t i = *off; i < size && base[i] && s.size() < 128; ++i) s += static_cast<char>(base[i]);
        return Lower(Widen(s));
    }
};

std::vector<std::wstring> Imports(const std::wstring& path) {
    std::vector<std::wstring> names;
    MappedFile file(path);
    if (!file.size()) return names;
    Pe pe{ file.data(), file.size() };
    auto dos = pe.At<IMAGE_DOS_HEADER>(0);
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE) return names;
    auto nt = pe.At<IMAGE_NT_HEADERS32>(static_cast<size_t>(dos->e_lfanew));
    if (!nt || nt->Signature != IMAGE_NT_SIGNATURE) return names;

    IMAGE_DATA_DIRECTORY imports{}, delay{};
    size_t optional = static_cast<size_t>(dos->e_lfanew) + offsetof(IMAGE_NT_HEADERS32, OptionalHeader);
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        auto nt64 = pe.At<IMAGE_NT_HEADERS64>(static_cast<size_t>(dos->e_lfanew));
        if (!nt64) return names;
        imports = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        delay = nt64->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    } else {
        imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        delay = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    }
    size_t sectionsAt = optional + nt->FileHeader.SizeOfOptionalHeader;
    pe.sectionCount = nt->FileHeader.NumberOfSections;
    if (sectionsAt + size_t(pe.sectionCount) * sizeof(IMAGE_SECTION_HEADER) > pe.size) return names;
    pe.sections = reinterpret_cast<const IMAGE_SECTION_HEADER*>(pe.base + sectionsAt);

    if (auto off = imports.VirtualAddress ? pe.Offset(imports.VirtualAddress) : std::nullopt)
        for (size_t i = 0; i < 512; ++i) {
            auto d = pe.At<IMAGE_IMPORT_DESCRIPTOR>(*off + i * sizeof(IMAGE_IMPORT_DESCRIPTOR));
            if (!d || d->Name == 0) break;
            names.push_back(pe.Name(d->Name));
        }
    if (auto off = delay.VirtualAddress ? pe.Offset(delay.VirtualAddress) : std::nullopt)
        for (size_t i = 0; i < 512; ++i) {
            auto d = pe.At<IMAGE_DELAYLOAD_DESCRIPTOR>(*off + i * sizeof(IMAGE_DELAYLOAD_DESCRIPTOR));
            if (!d || d->DllNameRVA == 0) break;
            names.push_back(pe.Name(d->DllNameRVA));
        }
    return names;
}

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

}  // namespace

uint32_t ScanExecutable(const std::wstring& exePath, std::wstring* engine) {
    // the import table and the folder around the exe do not change while it runs; rescans come with every re-probe
    static std::mutex gate;
    static std::map<std::wstring, std::pair<uint32_t, std::wstring>> cache;
    auto key = Lower(exePath);
    {
        std::lock_guard lock(gate);
        if (auto it = cache.find(key); it != cache.end()) {
            if (engine) *engine = it->second.second;
            return it->second.first;
        }
    }

    uint32_t flags = 0;
    for (auto& name : Imports(exePath)) flags |= ClassifyModule(name);

    auto dir = exePath.substr(0, exePath.find_last_of(L'\\'));
    auto stem = exePath.substr(dir.size() + 1);
    stem = stem.substr(0, stem.find_last_of(L'.'));
    auto found = [&](const wchar_t* name) { return Exists(dir + L"\\" + name); };
    std::wstring kind;
    if (found(L"UnityPlayer.dll") || found(L"GameAssembly.dll") || Exists(dir + L"\\" + stem + L"_Data")) kind = L"Unity";
    else if (Contains(exePath, L"\\Binaries\\Win64\\") || EndsWith(stem, L"-Win64-Shipping")) kind = L"Unreal Engine";
    else if (found(L"steam_api64.dll") || found(L"steam_api.dll")) kind = L"Steamworks";
    else if (found(L"EOSSDK-Win64-Shipping.dll")) kind = L"Epic Online Services";
    else if (found(L"nvngx_dlss.dll") || found(L"sl.interposer.dll") || found(L"libxess.dll")) kind = L"upscaler SDK";
    else if (found(L"bink2w64.dll") || found(L"fmod.dll") || found(L"fmodstudio.dll")) kind = L"game middleware";
    if (!kind.empty()) flags |= kModuleEngine;
    if (engine) *engine = kind;

    std::lock_guard lock(gate);
    if (cache.size() > 512) cache.clear();
    cache[key] = { flags, kind };
    return flags;
}

StaticFacts Analyze(const std::wstring& exePath, const std::wstring& parentExe, uint32_t loadedModules, const std::wstring& store, const Config& config) {
    StaticFacts s;
    auto name = FileName(exePath);
    for (auto& n : config.nonGames) if (name == n) s.nonGame = true;
    for (auto& d : config.gameDirs) if (Contains(exePath, d)) s.inGameDir = true;
    s.store = store;
    auto parent = FileName(parentExe);
    for (auto& l : config.launchers) if (!parent.empty() && parent == l) s.launcherChild = true;
    s.modules = loadedModules | ScanExecutable(exePath, &s.engine);
    return s;
}

Score Evaluate(const StaticFacts& s, const WindowFacts& w) {
    Score score;
    std::vector<std::wstring> why;
    auto add = [&](int points, const std::wstring& reason) { score.points += points; why.push_back(reason); };

    if (s.nonGame) { score.rejected = true; score.reasons = L"on the non-games list"; return score; }
    bool library = s.inGameDir || !s.store.empty();
    // Stop modules (CEF, Electron, WPF, WinForms) reject, unless the folder and engine both say "game":
    // a few games ship CEF for their menus.
    bool strong = (library || w.windowsGame) && (s.modules & kModuleEngine);
    if ((s.modules & kModuleStop) && !strong) { score.rejected = true; score.reasons = L"browser, Electron or desktop UI framework loaded"; return score; }

    if (w.windowsGame) add(3, L"in the Windows game list");
    if (!s.store.empty()) add(2, s.store + L" library");
    else if (s.inGameDir) add(2, L"game library folder");
    if (s.launcherChild) add(2, L"started by a game launcher");
    if (s.modules & kModuleEngine) add(2, s.engine.empty() ? L"game engine files" : s.engine);
    if (w.d3dFullscreen) add(2, L"exclusive fullscreen");
    else if (w.borderless && w.coversMonitor) add(1, L"borderless fullscreen");
    if (w.cursorHidden || w.cursorClipped) add(1, w.cursorClipped ? L"cursor locked to the window" : L"cursor hidden");
    if (s.modules & kModuleInput) add(1, L"game input API");
    if (s.modules & kModule3D) why.push_back(L"3D API");

    for (size_t i = 0; i < why.size(); ++i) score.reasons += (i ? L", " : L"") + why[i];
    return score;
}

// ------------------------------------------------------------------ anti-cheat

std::wstring DetectAntiCheat(const std::wstring& exePath) {
    static const std::array<std::pair<const wchar_t*, const wchar_t*>, 12> services = { {
        { L"EasyAntiCheat", L"Easy Anti-Cheat" }, { L"EasyAntiCheat_EOS", L"Easy Anti-Cheat" }, { L"BEService", L"BattlEye" },
        { L"vgc", L"Vanguard" }, { L"vgk", L"Vanguard" }, { L"FACEIT", L"FACEIT" }, { L"FACEITService", L"FACEIT" },
        { L"PnkBstrA", L"PunkBuster" }, { L"EAAntiCheatService", L"EA Javelin" }, { L"ACE-BASE", L"ACE" },
        { L"AntiCheatExpert Service", L"ACE" }, { L"GameGuard", L"nProtect GameGuard" },
    } };
    if (SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)) {
        std::wstring found;
        for (auto& [service, label] : services) {
            if (SC_HANDLE h = OpenServiceW(scm, service, SERVICE_QUERY_STATUS)) {
                SERVICE_STATUS status{};
                if (QueryServiceStatus(h, &status) && status.dwCurrentState == SERVICE_RUNNING) found = label;
                CloseServiceHandle(h);
            }
            if (!found.empty()) break;
        }
        CloseServiceHandle(scm);
        if (!found.empty()) return found;
    }
    // the folder tells too, even before the anti-cheat service starts
    auto dir = exePath.substr(0, exePath.find_last_of(L'\\'));
    for (int up = 0; up < 3 && dir.size() > 3; ++up) {
        if (Exists(dir + L"\\EasyAntiCheat") || Exists(dir + L"\\start_protected_game.exe")) return L"Easy Anti-Cheat";
        if (Exists(dir + L"\\BattlEye")) return L"BattlEye";
        dir = dir.substr(0, dir.find_last_of(L'\\'));
    }
    return {};
}

}  // namespace xww
