//  The app window: a borderless frame with its own title band and Windows 11
//  corners, a 240 px sidebar for navigation, pages on the black canvas,
//  dialogs for confirmations and choices, and toasts in the corner. Closing
//  it hides it to the notification area.
#pragma once

#include "agent/agent.h"
#include "ui/model.h"
#include "ui/widgets.h"

namespace xww::ui {

enum class Page { Overview, Optimizations, Games, Background, Logs, Settings };

class Window {
public:
    Window(Agent* agent, Model& model) : agent_(agent), model_(model) {}

    bool Create(bool visible);
    void Show();
    HWND Hwnd() const { return hwnd_; }
    void SetHideOnClose(bool on) { hideOnClose_ = on; }
    std::function<void(const ipc::Message&)> OnNotify;       // the tray shows these
    std::function<void()> OnStateChanged;                     // tray tooltip and menu

    // --render: draws one page into a PNG without a window
    bool RenderToFile(Page page, const std::wstring& path);

    void Toast(const std::wstring& text, Tone tone = Tone::On);
    void Confirm(std::wstring title, std::wstring message, std::wstring confirm, std::function<void()> action, bool destructive = true);
    // a list of the apps running with a window, plus a file dialog; `chosen` gets the exe path
    void Pick(std::wstring title, std::function<void(const std::wstring& exe)> chosen);

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnMessage(UINT msg, WPARAM wp, LPARAM lp);
    void Paint();
    void Draw(float width, float height);
    void DrawTitleBar(float width);
    void DrawSidebar(float height);
    void DrawPage(float width, float height);
    void DrawSheet(float width, float height);
    void DrawPicker(float width, float height);
    void DrawToasts(float width, float height);
    void DrawTooltip(float width, float height);
    bool Modal() const { return sheet_.has_value() || picker_.has_value(); }
    float BuildPage(Flow& f);                                  // returns the bottom of the content
    LRESULT HitTest(POINT screen);
    const Hit* HitAt(D2D1_POINT_2F p) const;
    void MoveFocus(bool back);
    void Navigate(Page page);
    void Invalidate() { if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE); }
    Theme CurrentTheme() const;
    void ApplyFrame();
    float Scale() const { return dpi_ / 96.f; }

    // pages (pages.cpp)
    void Overview(Flow& f);
    void Hero(Flow& f);
    void Optimizations(Flow& f);
    void Games(Flow& f);
    void Background(Flow& f);
    void Logs(Flow& f);
    void SettingsPage(Flow& f);
    void About(Flow& f);
    void AddGame(const wchar_t* list);
    std::wstring BrowseExe(const wchar_t* title);
    void EditList(const std::string& key, std::vector<std::wstring> names);
    void RunService(const wchar_t* arguments);
    void Uninstall();

    Agent* agent_;                  // null in --render
    Model& model_;
    HWND hwnd_ = nullptr;
    Canvas canvas_;
    Ui ui_{ canvas_ };
    Theme theme_{};
    ComPtr<ID2D1HwndRenderTarget> target_;
    UINT dpi_ = 96;
    Page page_ = Page::Overview;
    std::map<Page, float> scroll_;
    float contentHeight_ = 0, viewHeight_ = 0;
    D2D1_POINT_2F mouse_{ -1, -1 };
    bool hasMouse_ = false, tracking_ = false, hideOnClose_ = false, rendering_ = false;
    std::wstring pressed_, tipId_;
    ULONGLONG tipSince_ = 0;
    std::vector<std::wstring> logLines_;

    struct Sheet { std::wstring title, message, confirm; std::function<void()> action; bool destructive; };
    std::optional<Sheet> sheet_;
    struct PickItem { std::wstring name, exe; };
    struct Picker { std::wstring title; std::vector<PickItem> items; std::function<void(const std::wstring&)> chosen; float scroll = 0, contentHeight = 0, viewHeight = 0; };
    std::optional<Picker> picker_;
    struct ToastItem { std::wstring text; Tone tone; ULONGLONG until; };
    std::vector<ToastItem> toasts_;
};

}  // namespace xww::ui
