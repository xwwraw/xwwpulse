#include "detection/library.h"

#include <algorithm>
#include <regex>

namespace xww {

namespace {

constexpr ULONGLONG kRefreshMs = 5 * 60 * 1000;

std::string ReadSmallFile(const std::wstring& path) {
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr));
    LARGE_INTEGER size{};
    if (!file || !GetFileSizeEx(file.get(), &size) || size.QuadPart > (4 << 20)) return {};
    std::string data(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(file.get(), data.data(), static_cast<DWORD>(data.size()), &read, nullptr)) return {};
    data.resize(read);
    return data;
}

// JSON and VDF both escape a backslash as two
std::wstring Unescape(const std::string& text) {
    std::string out;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '\\') ++i;
        out += text[i];
    }
    return Widen(out);
}

std::vector<std::wstring> Matches(const std::string& text, const std::regex& pattern) {
    std::vector<std::wstring> found;
    for (std::sregex_iterator it(text.begin(), text.end(), pattern), end; it != end; ++it) found.push_back(Unescape((*it)[1].str()));
    return found;
}

}  // namespace

std::wstring GameLibrary::Store(const std::wstring& exePath) {
    std::lock_guard lock(gate_);
    if (!built_ || GetTickCount64() - built_ > kRefreshMs) Refresh();
    auto path = Lower(exePath);
    for (auto& [folder, store] : folders_)
        if (path.starts_with(folder)) return store;
    return {};
}

void GameLibrary::Add(const std::wstring& raw, const wchar_t* store) {
    auto folder = Lower(Trim(raw));
    std::replace(folder.begin(), folder.end(), L'/', L'\\');
    while (!folder.empty() && folder.back() == L'\\') folder.pop_back();
    if (folder.size() < 4 || folder[1] != L':') return;                 // never a whole drive
    folders_.emplace_back(folder + L"\\", store);
}

void GameLibrary::Refresh() {
    folders_.clear();
    built_ = GetTickCount64();
    const std::wstring wow = L"SOFTWARE\\WOW6432Node\\";

    // Steam: every library in libraryfolders.vdf
    if (auto steam = RegReadString(HKEY_LOCAL_MACHINE, wow + L"Valve\\Steam", L"InstallPath")) {
        static const std::regex path("\"path\"\\s+\"([^\"]+)\"");
        auto libraries = Matches(ReadSmallFile(*steam + L"\\steamapps\\libraryfolders.vdf"), path);
        libraries.push_back(*steam);
        for (auto& library : libraries) Add(library + L"\\steamapps\\common", L"Steam");
    }

    // Epic Games: one JSON manifest per installed game
    wchar_t programData[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"ProgramData", programData, MAX_PATH)) {
        static const std::regex location("\"InstallLocation\"\\s*:\\s*\"([^\"]+)\"");
        auto dir = std::wstring(programData) + L"\\Epic\\EpicGamesLauncher\\Data\\Manifests\\";
        WIN32_FIND_DATAW found{};
        HANDLE find = FindFirstFileW((dir + L"*.item").c_str(), &found);
        if (find != INVALID_HANDLE_VALUE) {
            do for (auto& folder : Matches(ReadSmallFile(dir + found.cFileName), location)) Add(folder, L"Epic Games");
            while (FindNextFileW(find, &found));
            FindClose(find);
        }
    }

    auto subkeys = [](const std::wstring& path, const wchar_t* value, const wchar_t* store, GameLibrary& self) {
        for (auto& name : RegSubkeyNames(HKEY_LOCAL_MACHINE, path))
            if (auto folder = RegReadString(HKEY_LOCAL_MACHINE, path + L"\\" + name, value)) self.Add(*folder, store);
    };
    subkeys(wow + L"GOG.com\\Games", L"path", L"GOG", *this);
    subkeys(wow + L"Ubisoft\\Launcher\\Installs", L"InstallDir", L"Ubisoft Connect", *this);
    subkeys(wow + L"EA Games", L"Install Dir", L"EA", *this);

    // Battle.net, Riot and Rockstar keep no manifest of their own that is easy to read; their uninstall entries name the folder
    static const std::pair<const wchar_t*, const wchar_t*> publishers[] = {
        { L"Blizzard Entertainment", L"Battle.net" }, { L"Riot Games", L"Riot" }, { L"Rockstar Games", L"Rockstar" },
        { L"Electronic Arts", L"EA" }, { L"Ubisoft", L"Ubisoft Connect" }, { L"GOG.com", L"GOG" },
    };
    for (auto uninstall : { std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall"), wow + L"Microsoft\\Windows\\CurrentVersion\\Uninstall" })
        for (auto& name : RegSubkeyNames(HKEY_LOCAL_MACHINE, uninstall)) {
            auto key = uninstall + L"\\" + name;
            auto publisher = RegReadString(HKEY_LOCAL_MACHINE, key, L"Publisher");
            if (!publisher) continue;
            for (auto& [match, store] : publishers)
                if (Contains(*publisher, match))
                    if (auto folder = RegReadString(HKEY_LOCAL_MACHINE, key, L"InstallLocation")) { Add(*folder, store); break; }
        }

    // launcher folders themselves are not games
    std::erase_if(folders_, [](auto& f) { return EndsWith(f.first, L"\\battle.net\\") || EndsWith(f.first, L"\\riot client\\") || EndsWith(f.first, L"\\launcher\\"); });
    Log(L"Detect", L"game library: {} install folders", folders_.size());
}

}  // namespace xww
