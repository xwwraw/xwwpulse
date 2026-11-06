#include "core/common.h"

#include <shlobj.h>
#include <algorithm>
#include <cwctype>
#include <mutex>
#include <cstdio>

namespace xww {

// ------------------------------------------------------------------ processes

Handle OpenLimited(DWORD pid, DWORD extraAccess) {
    return Handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | extraAccess, FALSE, pid));
}

uint64_t CreationTime(HANDLE process) {
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process || !GetProcessTimes(process, &created, &exited, &kernel, &user)) return 0;
    return (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
}

ProcessId Identify(DWORD pid) {
    auto h = OpenLimited(pid);
    return { pid, CreationTime(h.get()) };
}

std::wstring ImagePath(DWORD pid) {
    auto h = OpenLimited(pid);
    if (!h) return {};
    wchar_t buffer[MAX_PATH * 2];
    DWORD size = static_cast<DWORD>(std::size(buffer));
    return QueryFullProcessImageNameW(h.get(), 0, buffer, &size) ? std::wstring(buffer, size) : std::wstring();
}

bool SameProcess(const ProcessId& id) {
    auto h = OpenLimited(id.pid, SYNCHRONIZE);
    if (!h) return false;
    if (WaitForSingleObject(h.get(), 0) == WAIT_OBJECT_0) return false;
    return id.created == 0 || CreationTime(h.get()) == id.created;
}

// ------------------------------------------------------------------ strings

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

std::wstring FileName(std::wstring_view path) {
    auto slash = path.find_last_of(L"\\/");
    return Lower(slash == std::wstring_view::npos ? path : path.substr(slash + 1));
}

std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

std::string Narrow(std::wstring_view wide) {
    if (wide.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Trim(std::wstring_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::iswspace(s[a])) ++a;
    while (b > a && std::iswspace(s[b - 1])) --b;
    return std::wstring(s.substr(a, b - a));
}

bool EndsWith(std::wstring_view s, std::wstring_view suffix) {
    return s.size() >= suffix.size() && _wcsnicmp(s.data() + s.size() - suffix.size(), suffix.data(), suffix.size()) == 0;
}

bool Contains(std::wstring_view haystack, std::wstring_view needle) {
    return Lower(haystack).find(Lower(needle)) != std::wstring::npos;
}

std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep) {
    std::vector<std::wstring> parts;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == sep) { parts.emplace_back(s.substr(start, i - start)); start = i + 1; }
    return parts;
}

std::wstring GuidText(const GUID& g) {
    wchar_t buffer[64];
    StringFromGUID2(g, buffer, 64);
    return buffer;
}

std::optional<GUID> ParseGuid(std::wstring_view text) {
    GUID g{};
    std::wstring s(text);
    if (s.empty()) return std::nullopt;
    if (s.front() != L'{') s = L"{" + s + L"}";
    return SUCCEEDED(CLSIDFromString(s.c_str(), &g)) ? std::optional<GUID>(g) : std::nullopt;
}

// ------------------------------------------------------------------ paths

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring ProgramDataDir() {
    auto dir = KnownFolder(FOLDERID_ProgramData) + L"\\xwwPulse";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::wstring LogDir() {
    auto dir = KnownFolder(FOLDERID_ProgramData) + L"\\xwwPulse\\Logs";
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

std::wstring ModuleDir() {
    wchar_t buffer[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    std::wstring path(buffer, n);
    return path.substr(0, path.find_last_of(L'\\'));
}

// ------------------------------------------------------------------ log

namespace log {
static std::mutex gate;
static std::wstring file;
static bool echo = false;

void Init(const wchar_t* fileName) { std::lock_guard lock(gate); file = LogDir() + L"\\" + fileName; }
void SetConsole(bool on) { echo = on; }

void Write(std::wstring_view source, std::wstring_view message) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    auto line = std::format(L"[{:04}-{:02}-{:02} {:02}:{:02}:{:02}] {}: {}\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, source, message);
    OutputDebugStringW(line.c_str());
    std::lock_guard lock(gate);
    if (echo) { auto utf8 = Narrow(line); fwrite(utf8.data(), 1, utf8.size(), stdout); fflush(stdout); }
    if (file.empty()) return;
    // keep the log short: start over above 2 MB
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (GetFileAttributesExW(file.c_str(), GetFileExInfoStandard, &info) && info.nFileSizeLow > 2 * 1024 * 1024) DeleteFileW(file.c_str());
    Handle h(CreateFileW(file.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!h) return;
    auto utf8 = Narrow(line);
    DWORD written = 0;
    WriteFile(h.get(), utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
}
}  // namespace log

// ------------------------------------------------------------------ registry

std::optional<DWORD> RegReadDword(HKEY root, const std::wstring& path, const wchar_t* name) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return std::nullopt;
    return value;
}

std::optional<std::wstring> RegReadString(HKEY root, const std::wstring& path, const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS) return std::nullopt;
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(root, path.c_str(), name, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) return std::nullopt;
    value.resize(wcslen(value.c_str()));
    return value;
}

bool RegWriteDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD value) {
    return RegSetKeyValueW(root, path.c_str(), name, REG_DWORD, &value, sizeof(value)) == ERROR_SUCCESS;
}

bool RegWriteString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    return RegSetKeyValueW(root, path.c_str(), name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool RegDeleteValueAt(HKEY root, const std::wstring& path, const wchar_t* name) {
    return RegDeleteKeyValueW(root, path.c_str(), name) == ERROR_SUCCESS;
}

std::vector<std::wstring> RegValueNames(HKEY root, const std::wstring& path) {
    std::vector<std::wstring> names;
    RegKey key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS) return names;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumValueW(key.get(), i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        names.emplace_back(name, length);
    }
    return names;
}

std::vector<std::wstring> RegSubkeyNames(HKEY root, const std::wstring& path) {
    std::vector<std::wstring> names;
    RegKey key;
    if (RegOpenKeyExW(root, path.c_str(), 0, KEY_READ, key.put()) != ERROR_SUCCESS) return names;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD length = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(key.get(), i, name, &length, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        names.emplace_back(name, length);
    }
    return names;
}

std::wstring FriendlyName(const std::wstring& exe) {
    DWORD ignored = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &ignored);
    if (size) {
        std::vector<uint8_t> data(size);
        struct Translation { WORD language, codePage; } *tr = nullptr;
        UINT length = 0;
        if (GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) && VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&tr), &length) && length >= sizeof(Translation)) {
            for (auto field : { L"ProductName", L"FileDescription" }) {
                wchar_t* text = nullptr;
                auto query = std::format(L"\\StringFileInfo\\{:04x}{:04x}\\{}", tr->language, tr->codePage, field);
                if (VerQueryValueW(data.data(), query.c_str(), reinterpret_cast<void**>(&text), &length) && length > 1) {
                    auto name = Trim(text);
                    if (!name.empty()) return name;
                }
            }
        }
    }
    auto file = exe.substr(exe.find_last_of(L'\\') + 1);
    return file.substr(0, file.find_last_of(L'.'));
}

bool EnablePrivilege(const wchar_t* name) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &raw)) return false;
    Handle token(raw);
    TOKEN_PRIVILEGES tp{ 1 };
    if (!LookupPrivilegeValueW(nullptr, name, &tp.Privileges[0].Luid)) return false;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    return AdjustTokenPrivileges(token.get(), FALSE, &tp, sizeof(tp), nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
}

bool OnBattery() {
    SYSTEM_POWER_STATUS status{};
    return GetSystemPowerStatus(&status) && status.ACLineStatus == 0;
}

}  // namespace xww
