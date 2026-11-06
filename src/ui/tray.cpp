#include "ui/tray.h"

#include <commctrl.h>

namespace xww::ui {

static constexpr UINT WM_TRAY = WM_APP + 10;
enum : UINT { kOpen = 1, kPause, kExit };

bool Tray::Create(HINSTANCE instance) {
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = &Tray::Proc;
    wc.hInstance = instance;
    wc.lpszClassName = L"xwwPulseTray";
    RegisterClassExW(&wc);
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    if (!hwnd_) return false;
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");     // Explorer restarted: add the icon again

    data_.cbSize = sizeof(data_);
    data_.hWnd = hwnd_;
    data_.uID = 1;
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = WM_TRAY;
    LoadIconMetric(instance, MAKEINTRESOURCEW(1), LIM_SMALL, &data_.hIcon);
    wcscpy_s(data_.szTip, L"xwwPulse");
    Shell_NotifyIconW(NIM_ADD, &data_);
    data_.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data_);
    return true;
}

void Tray::Destroy() {
    if (!hwnd_) return;
    Shell_NotifyIconW(NIM_DELETE, &data_);
    if (data_.hIcon) DestroyIcon(data_.hIcon);
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void Tray::Update(const Model& m) {
    if (!hwnd_) return;
    paused_ = m.phase == L"off";
    std::wstring tip = !m.connected ? L"xwwPulse · service not running"
        : m.pid ? std::format(L"xwwPulse · {}{}", m.game, m.fps > 0 ? std::format(L" · {:.0f} fps", m.fps) : L"")
        : paused_ ? L"xwwPulse · paused" : L"xwwPulse · watching for games";
    wcsncpy_s(data_.szTip, tip.c_str(), _TRUNCATE);
    data_.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &data_);
    data_.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
}

void Tray::Notify(const std::wstring& title, const std::wstring& text) {
    if (!hwnd_) return;
    NOTIFYICONDATAW n = data_;
    n.uFlags = NIF_INFO;
    n.dwInfoFlags = NIIF_USER | NIIF_RESPECT_QUIET_TIME;
    wcsncpy_s(n.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void Tray::Menu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kOpen, L"Open xwwPulse");
    AppendMenuW(menu, MF_STRING, kPause, paused_ ? L"Turn game mode on" : L"Pause game mode");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, L"Exit");
    SetMenuDefaultItem(menu, kOpen, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(menu);
    if (cmd == kOpen && OnOpen) OnOpen();
    else if (cmd == kPause) { if (paused_) { if (OnResume) OnResume(); } else if (OnPause) OnPause(); }
    else if (cmd == kExit && OnExit) OnExit();
}

LRESULT CALLBACK Tray::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
    auto self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self && msg == self->taskbarCreated_ && msg) { Shell_NotifyIconW(NIM_ADD, &self->data_); Shell_NotifyIconW(NIM_SETVERSION, &self->data_); return 0; }
    if (self && msg == WM_TRAY) {
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT: if (self->OnOpen) self->OnOpen(); break;
        case WM_CONTEXTMENU: self->Menu(); break;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace xww::ui
