//  xwwPulse-Setup.exe: one file that installs, updates or removes xwwPulse.
//  It carries xwwPulseService.exe and xwwPulse.exe as resources and copies
//  them to Program Files\xwwPulse. xwwPulseService.exe --install then
//  registers the service and writes the default settings; a Start menu
//  shortcut opens the app. The setup keeps a copy of itself as uninstall.exe
//  in the install folder; started under that name, or with /uninstall, it
//  removes xwwPulse. An existing service is replaced wherever it was installed.
//    xwwPulse-Setup.exe               install or update, with a confirmation
//    xwwPulse-Setup.exe /silent       the same without dialogs
//    uninstall.exe                    remove the service, the files, the shortcut and the Apps entry
//    uninstall.exe /silent [/purge]   the same without dialogs; /purge also deletes settings and logs
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <string>
#include <vector>

namespace {

constexpr wchar_t kTitle[] = L"xwwPulse Setup";
constexpr wchar_t kUninstallKey[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\xwwPulse";
constexpr wchar_t kServiceName[] = L"xwwPulse";
constexpr wchar_t kService[] = L"xwwPulseService.exe";
constexpr wchar_t kControl[] = L"xwwPulse.exe";
constexpr wchar_t kSetup[] = L"xwwPulse-Setup.exe";
constexpr wchar_t kUninstaller[] = L"uninstall.exe";
constexpr int kServiceResource = 201, kControlResource = 202;

bool silent = false;

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path))) out = path;
    CoTaskMemFree(path);
    return out;
}

std::wstring InstallDir() { return KnownFolder(FOLDERID_ProgramFiles) + L"\\xwwPulse"; }
std::wstring ShortcutPath() { return KnownFolder(FOLDERID_CommonPrograms) + L"\\xwwPulse.lnk"; }

std::wstring SelfPath() {
    std::wstring path(MAX_PATH, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

std::wstring Folder(const std::wstring& path) { return path.substr(0, path.find_last_of(L'\\')); }

bool SamePath(const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A task dialog with one main button; returns true when it was chosen. Without a button it only informs.
// With `verify`, a checkbox below the text reports its state in `checked`.
bool Ask(const wchar_t* heading, const std::wstring& text, const wchar_t* button = nullptr, bool error = false, const wchar_t* verify = nullptr, bool* checked = nullptr) {
    if (silent) return true;
    TASKDIALOG_BUTTON buttons[] = { { IDOK, button } };
    TASKDIALOGCONFIG config{ sizeof(config) };
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = kTitle;
    config.hInstance = GetModuleHandleW(nullptr);
    config.pszMainIcon = error ? TD_ERROR_ICON : MAKEINTRESOURCEW(1);
    config.pszMainInstruction = heading;
    config.pszContent = text.c_str();
    if (button) { config.pButtons = buttons; config.cButtons = 1; config.dwCommonButtons = TDCBF_CANCEL_BUTTON; }
    else config.dwCommonButtons = TDCBF_CLOSE_BUTTON;
    config.pszVerificationText = verify;
    int chosen = 0;
    BOOL ticked = FALSE;
    TaskDialogIndirect(&config, &chosen, nullptr, verify ? &ticked : nullptr);
    if (checked) *checked = ticked != FALSE;
    return chosen == IDOK;
}

int Fail(const std::wstring& what, DWORD code) {
    Ask(L"xwwPulse could not be installed", what + L"\n\nClose xwwPulse if it is open and run the setup again. Error code: " + std::to_wstring(code) + L".", nullptr, true);
    return static_cast<int>(code ? code : 1);
}

// Runs a command hidden and returns its exit code.
DWORD Run(const std::wstring& exe, const std::wstring& arguments) {
    std::wstring command = L"\"" + exe + L"\" " + arguments;
    STARTUPINFOW startup{ sizeof(startup) };
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return GetLastError();
    WaitForSingleObject(process.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return code;
}

void CloseApp() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W entry{ sizeof(entry) };
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (CompareStringOrdinal(entry.szExeFile, -1, kControl, -1, TRUE) != CSTR_EQUAL) continue;
        if (HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID)) {
            TerminateProcess(p, 0);
            WaitForSingleObject(p, 5000);
            CloseHandle(p);
        }
    }
    CloseHandle(snapshot);
}

// The folder of the installed service, read from its configuration, so an update finds any earlier install.
std::wstring ServiceFolder() {
    std::wstring folder;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return folder;
    if (SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_CONFIG)) {
        DWORD needed = 0;
        QueryServiceConfigW(svc, nullptr, 0, &needed);
        std::vector<BYTE> buffer(needed);
        auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
        if (needed && QueryServiceConfigW(svc, config, needed, &needed) && config->lpBinaryPathName) {
            std::wstring path = config->lpBinaryPathName;
            if (!path.empty() && path.front() == L'"') path = path.substr(1, path.find(L'"', 1) - 1);
            folder = Folder(path);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return folder;
}

// Removes an installed service first: its own --uninstall stops it and undoes anything it still had applied.
// Returns the folder it was installed in, or empty.
std::wstring StopExisting() {
    auto folder = ServiceFolder();
    if (!folder.empty() && Exists(folder + L"\\" + kService)) Run(folder + L"\\" + kService, L"--uninstall");
    CloseApp();
    return folder;
}

void RemoveFiles(const std::wstring& dir, bool& pending) {
    auto self = SelfPath();
    for (auto name : { kService, kControl, kSetup, kUninstaller }) {
        auto path = dir + L"\\" + name;
        if (!Exists(path)) continue;
        if (SamePath(path, self) || !DeleteFileW(path.c_str())) pending |= MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
    }
    // only an empty folder goes; anything else in it stays where it is
    if (!RemoveDirectoryW(dir.c_str()) && GetLastError() != ERROR_DIR_NOT_EMPTY) pending |= MoveFileExW(dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT) != FALSE;
}

// Writes a resource to a file; a file that is still locked by a stopping service is retried for a few seconds.
DWORD Extract(int id, const std::wstring& target) {
    HRSRC info = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL data = info ? LoadResource(nullptr, info) : nullptr;
    if (!data) return GetLastError();
    auto bytes = static_cast<const BYTE*>(LockResource(data));
    DWORD size = SizeofResource(nullptr, info), written = 0;
    auto temporary = target + L".new";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    BOOL ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    CloseHandle(file);
    if (!ok) { DeleteFileW(temporary.c_str()); return ERROR_WRITE_FAULT; }
    for (int attempt = 0; attempt < 20; ++attempt) {
        if (MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return ERROR_SUCCESS;
        Sleep(500);
    }
    DWORD code = GetLastError();
    DeleteFileW(temporary.c_str());
    return code;
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

void SetDword(HKEY key, const wchar_t* name, DWORD value) { RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)); }

DWORD FolderSizeKb(const std::wstring& dir) {
    ULONGLONG total = 0;
    WIN32_FIND_DATAW found{};
    HANDLE find = FindFirstFileW((dir + L"\\*.exe").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) return 0;
    do total += (static_cast<ULONGLONG>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
    while (FindNextFileW(find, &found));
    FindClose(find);
    return static_cast<DWORD>(total / 1024);
}

void RegisterApp(const std::wstring& dir) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    auto setup = L"\"" + dir + L"\\" + kUninstaller + L"\"";
    SetString(key, L"DisplayName", L"xwwPulse");
    SetString(key, L"DisplayVersion", XWW_VERSION);
    SetString(key, L"Publisher", L"xwwraw");
    SetString(key, L"DisplayIcon", dir + L"\\" + kControl);
    SetString(key, L"InstallLocation", dir);
    SetString(key, L"URLInfoAbout", L"https://github.com/xwwraw/xwwpulse");
    SetString(key, L"UninstallString", setup);
    SetString(key, L"QuietUninstallString", setup + L" /silent");
    SetDword(key, L"NoModify", 1);
    SetDword(key, L"NoRepair", 1);
    SetDword(key, L"EstimatedSize", FolderSizeKb(dir));
    RegCloseKey(key);
}

HRESULT CreateShortcut(const std::wstring& target) {
    IShellLinkW* link = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link));
    if (FAILED(hr)) return hr;
    link->SetPath(target.c_str());
    link->SetWorkingDirectory(Folder(target).c_str());
    link->SetDescription(L"Game mode for Windows");
    link->SetIconLocation(target.c_str(), 0);
    IPersistFile* file = nullptr;
    hr = link->QueryInterface(IID_PPV_ARGS(&file));
    if (SUCCEEDED(hr)) { hr = file->Save(ShortcutPath().c_str(), TRUE); file->Release(); }
    link->Release();
    return hr;
}

// The setup runs elevated; opening the app through Explorer starts it as the signed-in user instead.
void OpenApp(const std::wstring& dir) {
    auto app = dir + L"\\" + kControl;
    ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + app + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

int Install() {
    auto dir = InstallDir();
    if (!Ask(L"Install xwwPulse " XWW_VERSION L"?",
            L"xwwPulse runs as a Windows service. While a game is in front it raises its CPU and GPU priority, keeps the cores out of deep sleep "
            L"and moves background apps aside, and it undoes each change when the game closes.\n\nIt is installed to " + dir + L". An earlier version is replaced; your settings are kept.",
            L"Install"))
        return ERROR_CANCELLED;

    int created = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (created != ERROR_SUCCESS && created != ERROR_ALREADY_EXISTS) return Fail(L"The folder " + dir + L" could not be created.", static_cast<DWORD>(created));
    auto previous = StopExisting();
    if (DWORD code = Extract(kServiceResource, dir + L"\\" + kService)) return Fail(L"xwwPulseService.exe could not be written.", code);
    if (DWORD code = Extract(kControlResource, dir + L"\\" + kControl)) return Fail(L"xwwPulse.exe could not be written.", code);
    auto self = SelfPath(), setup = dir + L"\\" + kUninstaller;
    if (!SamePath(self, setup) && !CopyFileW(self.c_str(), setup.c_str(), FALSE)) return Fail(L"The setup could not be kept for uninstalling.", GetLastError());
    if (DWORD code = Run(dir + L"\\" + kService, L"--install")) return Fail(L"The xwwPulse service could not be registered.", code);
    bool pending = false;
    if (!previous.empty() && !SamePath(previous, dir)) RemoveFiles(previous, pending);
    CreateShortcut(dir + L"\\" + kControl);
    RegisterApp(dir);

    if (!silent) {
        OpenApp(dir);
        Ask(L"xwwPulse " XWW_VERSION L" is installed",
            L"The service is running and xwwPulse is in the notification area. Both start with Windows; open xwwPulse from the Start menu, and remove it in Settings > Apps.");
    }
    return 0;
}

// Settings, logs and the rollback journal; the journal is empty once the service has undone its changes.
void Purge() {
    auto data = KnownFolder(FOLDERID_ProgramData) + L"\\xwwPulse";
    data.push_back(L'\0');                      // SHFileOperation takes a list ending in two nulls
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = data.c_str();
    operation.fFlags = FOF_NO_UI;
    SHFileOperationW(&operation);
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\xwwPulse");
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\xwwPulse", KEY_WOW64_64KEY, 0);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\xwwPulse");
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\xwwPulse");
}

int Uninstall(bool purge) {
    auto dir = InstallDir();
    if (!Ask(L"Remove xwwPulse?", L"The service is stopped and removed, and every change it still has applied is undone. The app, its shortcut and its entry in Settings > Apps are removed too.",
            L"Remove", false, L"Also delete my settings and logs", &purge))
        return ERROR_CANCELLED;
    auto previous = StopExisting();
    bool pending = false;
    RemoveFiles(dir, pending);
    if (!previous.empty() && !SamePath(previous, dir)) RemoveFiles(previous, pending);
    DeleteFileW(ShortcutPath().c_str());
    RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, KEY_WOW64_64KEY, 0);
    if (purge) Purge();
    Ask(L"xwwPulse is removed", pending ? L"The service is gone. Its last files are deleted at the next restart." : purge ? L"xwwPulse, its settings and its logs are gone." : L"The service and its files are gone. Your settings stay in C:\\ProgramData\\xwwPulse.");
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // the copy in the install folder is named uninstall.exe and removes xwwPulse when started
    auto self = SelfPath();
    bool uninstall = SamePath(self.substr(self.find_last_of(L'\\') + 1), kUninstaller), purge = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], L"/silent") == 0 || _wcsicmp(argv[i], L"/verysilent") == 0) silent = true;
        else if (_wcsicmp(argv[i], L"/uninstall") == 0) uninstall = true;
        else if (_wcsicmp(argv[i], L"/purge") == 0) purge = true;
    }
    LocalFree(argv);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int code = uninstall ? Uninstall(purge) : Install();
    CoUninitialize();
    return code;
}
