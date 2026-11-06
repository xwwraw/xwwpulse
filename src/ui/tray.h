//  Notification-area icon: left click opens the dashboard, the menu pauses
//  or resumes game mode and exits. "Game mode on" notifications go through
//  it as Windows notifications.
#pragma once

#include "ui/model.h"

#include <shellapi.h>

namespace xww::ui {

class Tray {
public:
    std::function<void()> OnOpen, OnExit, OnPause, OnResume;

    bool Create(HINSTANCE instance);
    void Destroy();
    void Update(const Model& model);                         // tooltip
    void Notify(const std::wstring& title, const std::wstring& text);
    bool Paused() const { return paused_; }

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void Menu();

    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW data_{};
    UINT taskbarCreated_ = 0;
    bool paused_ = false;
};

}  // namespace xww::ui
