//  Shared basics: RAII handles, string helpers, paths, the log and the
//  process identity (PID + creation time) every module keys its state by.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <functional>
#include <format>

#ifndef XWW_VERSION
#define XWW_VERSION L"1.0.0"
#endif

namespace xww {

inline constexpr wchar_t kProduct[] = L"xwwPulse";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\xwwPulse";
inline constexpr wchar_t kServiceName[] = L"xwwPulse";
inline constexpr wchar_t kStateKey[] = L"SOFTWARE\\xwwPulse";                    // HKLM: service state and journal
inline constexpr wchar_t kUserKey[] = L"Software\\xwwPulse";                     // HKCU: per-user lists and UI settings

// ------------------------------------------------------------------ handles

class Handle {
public:
    Handle() = default;
    explicit Handle(HANDLE h) : h_(h == INVALID_HANDLE_VALUE ? nullptr : h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h_(o.h_) { o.h_ = nullptr; }
    Handle& operator=(Handle&& o) noexcept { if (this != &o) { reset(); h_ = o.h_; o.h_ = nullptr; } return *this; }
    ~Handle() { reset(); }
    void reset(HANDLE h = nullptr) { if (h_) CloseHandle(h_); h_ = h == INVALID_HANDLE_VALUE ? nullptr : h; }
    HANDLE get() const { return h_; }
    HANDLE release() { auto h = h_; h_ = nullptr; return h; }
    explicit operator bool() const { return h_ != nullptr; }
private:
    HANDLE h_ = nullptr;
};

class RegKey {
public:
    RegKey() = default;
    explicit RegKey(HKEY k) : k_(k) {}
    RegKey(const RegKey&) = delete;
    RegKey& operator=(const RegKey&) = delete;
    RegKey(RegKey&& o) noexcept : k_(o.k_) { o.k_ = nullptr; }
    ~RegKey() { if (k_) RegCloseKey(k_); }
    HKEY get() const { return k_; }
    HKEY* put() { if (k_) { RegCloseKey(k_); k_ = nullptr; } return &k_; }
    explicit operator bool() const { return k_ != nullptr; }
private:
    HKEY k_ = nullptr;
};

// ------------------------------------------------------------------ process identity

// A PID alone is reused within seconds; PID + creation time names one process for its whole life.
struct ProcessId {
    DWORD pid = 0;
    uint64_t created = 0;   // FILETIME as 100 ns ticks
    bool operator==(const ProcessId&) const = default;
    bool valid() const { return pid != 0; }
};

struct ProcessIdHash {
    size_t operator()(const ProcessId& p) const noexcept { return std::hash<uint64_t>{}((uint64_t(p.pid) << 40) ^ p.created); }
};

// Opens with only the rights Task Manager uses; never VM read/write, never thread access.
Handle OpenLimited(DWORD pid, DWORD extraAccess = 0);
uint64_t CreationTime(HANDLE process);
ProcessId Identify(DWORD pid);
std::wstring ImagePath(DWORD pid);                         // full Win32 path, or empty
bool SameProcess(const ProcessId& id);                     // still alive and not a reused PID
std::wstring FriendlyName(const std::wstring& exe);        // ProductName / FileDescription, else the file name

// ------------------------------------------------------------------ strings

std::wstring Lower(std::wstring_view s);
std::wstring FileName(std::wstring_view path);             // "C:\x\Game.exe" -> "game.exe" (lower case)
std::wstring Widen(std::string_view utf8);
std::string Narrow(std::wstring_view wide);
std::wstring Trim(std::wstring_view s);
bool EndsWith(std::wstring_view s, std::wstring_view suffix);
bool Contains(std::wstring_view haystack, std::wstring_view needle);   // case-insensitive
std::vector<std::wstring> Split(std::wstring_view s, wchar_t sep);
std::wstring GuidText(const GUID& g);
std::optional<GUID> ParseGuid(std::wstring_view text);

// ------------------------------------------------------------------ paths

std::wstring ProgramDataDir();                             // C:\ProgramData\xwwPulse
std::wstring LogDir();                                     // C:\ProgramData\xwwPulse\Logs
std::wstring ModuleDir();                                  // folder of the running exe

// ------------------------------------------------------------------ log

namespace log {
void Init(const wchar_t* fileName);                        // "Pulse.log" or "PulseDashboard.log"
void Write(std::wstring_view source, std::wstring_view message);
void SetConsole(bool echo);
}

template <class... Args>
void Log(std::wstring_view source, std::wformat_string<Args...> fmt, Args&&... args) {
    log::Write(source, std::format(fmt, std::forward<Args>(args)...));
}

// ------------------------------------------------------------------ registry

std::optional<DWORD> RegReadDword(HKEY root, const std::wstring& path, const wchar_t* name);
std::optional<std::wstring> RegReadString(HKEY root, const std::wstring& path, const wchar_t* name);
bool RegWriteDword(HKEY root, const std::wstring& path, const wchar_t* name, DWORD value);
bool RegWriteString(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value);
bool RegDeleteValueAt(HKEY root, const std::wstring& path, const wchar_t* name);
std::vector<std::wstring> RegValueNames(HKEY root, const std::wstring& path);
std::vector<std::wstring> RegSubkeyNames(HKEY root, const std::wstring& path);

bool EnablePrivilege(const wchar_t* name);
bool OnBattery();

}  // namespace xww
