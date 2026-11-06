#include "ui/window.h"
#include "ui/icons.h"

#include <algorithm>
#include <dwmapi.h>
#include <windowsx.h>

namespace xww::ui {

using namespace metric;

static constexpr UINT_PTR kAnimationTimer = 1, kTooltipTimer = 2;
static constexpr ULONGLONG kTooltipDelayMs = 400;
static constexpr wchar_t kClass[] = L"xwwPulseDashboard";

// ------------------------------------------------------------------ appearance

Theme Window::CurrentTheme() const {
    Theme t;
    if (rendering_) t.reduceMotion = true;          // a still image shows every animation at its end
    else {
        BOOL animate = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
        t.reduceMotion = !animate;
    }
    return t;
}

// One dark theme: a dark frame, rounded corners and the DWM shadow for a window without a system frame.
void Window::ApplyFrame() {
    if (!hwnd_) return;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int round = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd_, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
    MARGINS margins{ 1, 1, 1, 1 };
    DwmExtendFrameIntoClientArea(hwnd_, &margins);
}

// ------------------------------------------------------------------ window

bool Window::Create(bool visible) {
    if (!canvas_.Init()) return false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &Window::Proc;
    wc.hInstance = instance;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(0, kClass, L"xwwPulse", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1120, 760, nullptr, nullptr, instance, this);
    if (!hwnd_) return false;
    dpi_ = GetDpiForWindow(hwnd_);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY), &mi);
    int w = static_cast<int>(1120 * Scale()), h = static_cast<int>(760 * Scale());
    SetWindowPos(hwnd_, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2, w, h, SWP_NOZORDER | SWP_FRAMECHANGED);
    ApplyFrame();
    if (agent_) agent_->Start(hwnd_);
    if (visible) Show();
    return true;
}

void Window::Show() {
    ShowWindow(hwnd_, IsIconic(hwnd_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(hwnd_);
    Invalidate();
}

LRESULT CALLBACK Window::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        auto self = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    auto self = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return self ? self->OnMessage(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

const Hit* Window::HitAt(D2D1_POINT_2F p) const {
    auto& hits = ui_.Hits();
    for (auto it = hits.rbegin(); it != hits.rend(); ++it)
        if (Inside(it->rect, p)) return &*it;
    return nullptr;
}

LRESULT Window::HitTest(POINT screen) {
    POINT pt = screen;
    ScreenToClient(hwnd_, &pt);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!IsZoomed(hwnd_)) {
        int b = static_cast<int>(6 * Scale());
        bool left = pt.x < b, right = pt.x >= rc.right - b, top = pt.y < b, bottom = pt.y >= rc.bottom - b;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    D2D1_POINT_2F p{ pt.x / Scale(), pt.y / Scale() };
    if (p.y < TitleBarHeight && !Modal() && !HitAt(p)) return HTCAPTION;
    return HTCLIENT;
}

// Tab and Shift+Tab walk the controls in drawing order; the ring shows only after the keyboard was used.
void Window::MoveFocus(bool back) {
    std::vector<const Hit*> stops;
    for (auto& h : ui_.Hits())
        if (h.action && (!Modal() || h.id.starts_with(L"sheet/")) && h.id != L"sheet/shade") stops.push_back(&h);
    if (stops.empty()) return;
    auto it = std::find_if(stops.begin(), stops.end(), [&](const Hit* h) { return h->id == ui_.Focus(); });
    size_t i = it == stops.end() ? (back ? stops.size() - 1 : 0) : (it - stops.begin() + (back ? stops.size() - 1 : 1)) % stops.size();
    ui_.SetFocus(stops[i]->id);
    // keep the focused control in view
    auto& r = stops[i]->rect;
    float& s = scroll_[page_];
    if (r.left > SidebarWidth && r.top >= TitleBarHeight) {
        if (r.bottom > TitleBarHeight + viewHeight_ - Space24) s += r.bottom - (TitleBarHeight + viewHeight_ - Space24);
    } else if (r.left > SidebarWidth && r.bottom > 0 && r.top < TitleBarHeight + Space16) s -= TitleBarHeight + Space16 - r.top;
    Invalidate();
}

LRESULT Window::OnMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (wp) {
            if (IsZoomed(hwnd_)) {
                auto& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)->rgrc[0];
                int frame = GetSystemMetricsForDpi(SM_CXFRAME, dpi_) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi_);
                r.left += frame; r.top += frame; r.right -= frame; r.bottom -= frame;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return HitTest({ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) });
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(lp);
        info->ptMinTrackSize = { static_cast<LONG>(900 * Scale()), static_cast<LONG>(580 * Scale()) };
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = HIWORD(wp);
        auto r = reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (target_) target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        canvas_.ReleaseDeviceResources();
        Invalidate();
        return 0;
    }
    case WM_SIZE:
        if (target_) target_->Resize({ LOWORD(lp), HIWORD(lp) });
        Invalidate();
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_SETTINGCHANGE:
        if (lp && (wcscmp(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0)) { ApplyFrame(); Invalidate(); }
        return 0;

    case WM_MOUSEMOVE: {
        mouse_ = { GET_X_LPARAM(lp) / Scale(), GET_Y_LPARAM(lp) / Scale() };
        hasMouse_ = true;
        if (!tracking_) { TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd_ }; TrackMouseEvent(&t); tracking_ = true; }
        auto hit = HitAt(mouse_);
        std::wstring id = hit ? hit->id : L"";
        if (id != tipId_) {
            bool hadTip = !tipId_.empty() && GetTickCount64() - tipSince_ >= kTooltipDelayMs;
            tipId_ = id;
            tipSince_ = GetTickCount64();
            if (hit && !hit->tooltip.empty()) SetTimer(hwnd_, kTooltipTimer, static_cast<UINT>(kTooltipDelayMs), nullptr);
            if (hadTip) Invalidate();
        }
        if (id != ui_.Hot()) Invalidate();
        return 0;
    }
    case WM_MOUSELEAVE: hasMouse_ = tracking_ = false; tipId_.clear(); Invalidate(); return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, HitAt(mouse_) ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_LBUTTONDOWN: {
        auto hit = HitAt(mouse_);
        pressed_ = hit ? hit->id : L"";
        ui_.SetPressed(pressed_);
        ui_.SetFocus(L"");
        tipId_.clear();
        SetCapture(hwnd_);
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        auto hit = HitAt(mouse_);
        std::function<void()> action;
        if (hit && hit->id == pressed_ && !pressed_.empty()) action = hit->action;
        pressed_.clear();
        ui_.SetPressed(L"");
        if (action) action();
        Invalidate();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (picker_) {
            picker_->scroll = std::clamp(picker_->scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48.f, 0.f, std::max(0.f, picker_->contentHeight - picker_->viewHeight));
            Invalidate();
            return 0;
        }
        if (sheet_) return 0;
        float& s = scroll_[page_];
        s = std::clamp(s - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 48.f, 0.f, std::max(0.f, contentHeight_ - viewHeight_));
        Invalidate();
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (wp == VK_ESCAPE && Modal()) { sheet_.reset(); picker_.reset(); ui_.SetFocus(L""); Invalidate(); }
        else if (wp == VK_TAB) MoveFocus(GetKeyState(VK_SHIFT) < 0);
        else if ((wp == VK_RETURN || wp == VK_SPACE) && !ui_.Focus().empty()) {
            for (auto& h : ui_.Hits())
                if (h.id == ui_.Focus() && h.action) { auto action = h.action; action(); Invalidate(); break; }
        } else if (ctrl && wp == VK_OEM_COMMA) Navigate(Page::Settings);
        return 0;
    }
    case WM_TIMER:
        if (wp == kAnimationTimer) Invalidate();
        else if (wp == kTooltipTimer) { KillTimer(hwnd_, kTooltipTimer); Invalidate(); }
        else if (agent_) agent_->OnTimer(wp);
        return 0;

    case WM_PULSE: {
        std::unique_ptr<ipc::Message> m(reinterpret_cast<ipc::Message*>(lp));
        if (agent_) agent_->OnMessage(*m);
        model_.Apply(*m);
        if (m->type == "notify" && OnNotify) OnNotify(*m);
        if (OnStateChanged) OnStateChanged();
        if (IsWindowVisible(hwnd_)) Invalidate();
        return 0;
    }
    case WM_PULSE_CONNECTION:
        model_.connected = wp != 0;
        if (!model_.connected) { model_.phase = L"idle"; model_.steps.clear(); model_.pid = 0; model_.fpsSeries.clear(); }
        if (agent_) agent_->OnConnection(model_.connected);
        if (OnStateChanged) OnStateChanged();
        Invalidate();
        return 0;

    case WM_CLOSE:
        if (hideOnClose_) { ShowWindow(hwnd_, SW_HIDE); return 0; }
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        if (agent_) agent_->Stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

// ------------------------------------------------------------------ drawing

void Window::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd_, &ps);
    EndPaint(hwnd_, &ps);
    RECT rc;
    GetClientRect(hwnd_, &rc);
    if (!target_) {
        auto props = D2D1::RenderTargetProperties();
        props.dpiX = props.dpiY = static_cast<float>(dpi_);
        if (FAILED(canvas_.Factory()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd_, { static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom) }), target_.GetAddressOf()))) return;
        canvas_.ReleaseDeviceResources();
    }
    target_->BeginDraw();
    canvas_.Begin(target_.Get(), Scale());
    Draw(rc.right / Scale(), rc.bottom / Scale());
    canvas_.End();
    if (target_->EndDraw() == D2DERR_RECREATE_TARGET) { target_.Reset(); canvas_.ReleaseDeviceResources(); Invalidate(); }

    if (ui_.Animating()) SetTimer(hwnd_, kAnimationTimer, 16, nullptr);
    else if (!toasts_.empty()) SetTimer(hwnd_, kAnimationTimer, 100, nullptr);
    else KillTimer(hwnd_, kAnimationTimer);
}

void Window::Draw(float width, float height) {
    theme_ = CurrentTheme();
    ui_.Begin(theme_, mouse_, hasMouse_ && !Modal(), GetTickCount64());
    canvas_.Clear(color::Canvas);
    DrawPage(width, height);
    DrawSidebar(height);
    DrawTitleBar(width);
    if (picker_) { ui_.ClearOffset(); ui_.SetHasMouse(hasMouse_); DrawPicker(width, height); }
    if (sheet_) { ui_.ClearOffset(); ui_.SetHasMouse(hasMouse_); DrawSheet(width, height); }
    DrawToasts(width, height);
    ui_.DrawFocusRing();
    DrawTooltip(width, height);
}

// The title band across the window: nav color, the wordmark, the window buttons and a hairline below.
void Window::DrawTitleBar(float width) {
    canvas_.Fill(Rect(0, 0, width, TitleBarHeight), color::Nav);
    canvas_.Fill(Rect(0, TitleBarHeight - 1, width, 1), color::Hairline);
    float logoHeight = 22;
    canvas_.Wordmark(Rect(Space24, (TitleBarHeight - logoHeight) / 2, logoHeight * 1525.f / 449.f, logoHeight));

    float x = width - CaptionButtonWidth * 3, y = (TitleBarHeight - 1 - CaptionButtonHeight) / 2;
    bool zoomed = hwnd_ && IsZoomed(hwnd_);
    ui_.CaptionButton(Rect(x, y, CaptionButtonWidth, CaptionButtonHeight), icons::Minimize, [this] { ShowWindow(hwnd_, SW_MINIMIZE); }, L"caption/min", L"Minimize");
    ui_.CaptionButton(Rect(x + CaptionButtonWidth, y, CaptionButtonWidth, CaptionButtonHeight), zoomed ? icons::Restore : icons::Maximize,
        [this] { ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); }, L"caption/max", zoomed ? L"Restore" : L"Maximize");
    ui_.CaptionButton(Rect(x + CaptionButtonWidth * 2, y, CaptionButtonWidth, CaptionButtonHeight), icons::Close, [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); }, L"caption/close", L"Close");
}

void Window::DrawSidebar(float height) {
    canvas_.Fill(Rect(0, TitleBarHeight, SidebarWidth, height - TitleBarHeight), color::Nav);
    canvas_.Fill(Rect(SidebarWidth - 1, TitleBarHeight, 1, height - TitleBarHeight), color::Hairline);

    float y = TitleBarHeight + Space16;
    auto nav = [&](Page page, const char* icon, const wchar_t* label) {
        ui_.Nav(Rect(Space12, y, SidebarWidth - Space24, ControlMd), icon, label, page_ == page, [this, page] { Navigate(page); }, std::wstring(L"nav/") + label);
        y += ControlMd + Space4;
    };
    auto header = [&](const wchar_t* text) {
        y += Space16;
        canvas_.Text(text, Rect(Space24, y, 200, Spec(Type::Caption).line), color::TextSubtle, Style(Type::Caption));
        y += Spec(Type::Caption).line + Space8;
    };
    nav(Page::Overview, icons::Home, L"Overview");
    header(L"Game mode");
    nav(Page::Optimizations, icons::Optimize, L"Optimizations");
    nav(Page::Games, icons::Games, L"Games");
    nav(Page::Background, icons::Background, L"Background");
    header(L"Maintenance");
    nav(Page::Logs, icons::Logs, L"Logs");

    // the state above Settings, told with an icon and words
    float settingsTop = height - Space12 - ControlMd;
    D2D1_RECT_F status = Rect(Space12, settingsTop - Space8 - 56, SidebarWidth - Space24, 56);
    bool hover = ui_.Interact(L"status", status, [this] { Navigate(model_.connected ? Page::Overview : Page::Settings); }, true, RadiusSm);
    if (hover) canvas_.Fill(status, color::Hover, RadiusSm);
    const char* icon = icons::Off;
    std::wstring title, detail;
    if (!model_.connected) { icon = icons::Alert; title = L"Service not running"; detail = L"Install it in Settings"; }
    else if (model_.phase == L"game") { icon = icons::Zap; title = L"Game mode on"; detail = model_.game + (model_.fps > 0 ? std::format(L" · {:.0f} fps", model_.fps) : L""); }
    else if (model_.phase == L"paused") { icon = icons::Pause; title = L"Game in the background"; detail = model_.game; }
    else if (model_.phase == L"confirming") { icon = icons::Refresh; title = L"Checking the frame rate"; detail = L"A possible game is in front"; }
    else if (model_.phase == L"off" || model_.phase == L"disabled") { icon = icons::Pause; title = L"Game mode off"; detail = L"Turn it on in Overview"; }
    else { icon = icons::Check; title = L"Watching for games"; detail = L"No game in front"; }
    canvas_.Icon(icon, { status.left + Space12, status.top + Space8 + 2 }, IconSm, color::Text);
    canvas_.Text(title, Rect(status.left + Space12 + IconSm + Space12, status.top + Space8, Width(status) - 48, 20), color::Text, Style(Type::BodySm));
    canvas_.Text(detail, Rect(status.left + Space12 + IconSm + Space12, status.top + Space8 + 20, Width(status) - 48, 18), color::TextMuted, Style(Type::Caption));

    ui_.Nav(Rect(Space12, settingsTop, SidebarWidth - Space24, ControlMd), icons::Settings, L"Settings", page_ == Page::Settings, [this] { Navigate(Page::Settings); }, L"nav/Settings");
}

void Window::DrawPage(float width, float height) {
    float avail = width - SidebarWidth;
    float host = std::min(PageWidth, avail - PageGutter * 2);
    float left = SidebarWidth + (avail - host) / 2;
    D2D1_RECT_F clip{ SidebarWidth, TitleBarHeight, width, height };
    viewHeight_ = height - TitleBarHeight;
    float& scroll = scroll_[page_];
    scroll = std::clamp(scroll, 0.f, std::max(0.f, contentHeight_ - viewHeight_));

    canvas_.PushClip(clip);
    canvas_.Translate(0, -scroll);
    ui_.SetOffset(0, -scroll, clip);
    Flow f{ left, TitleBarHeight + Space40, host };
    contentHeight_ = BuildPage(f) - TitleBarHeight + Space24;
    canvas_.ResetTransform();
    ui_.ClearOffset();
    canvas_.PopClip();

    if (contentHeight_ > viewHeight_) {
        float track = viewHeight_ - Space8;
        float thumb = std::max(24.f, track * viewHeight_ / contentHeight_);
        float y = TitleBarHeight + Space4 + (track - thumb) * scroll / (contentHeight_ - viewHeight_);
        canvas_.Fill(Rect(width - Space8, y, 4, thumb), color::Smoke, 2);
    }
}

float Window::BuildPage(Flow& f) {
    switch (page_) {
    case Page::Overview: Overview(f); break;
    case Page::Optimizations: Optimizations(f); break;
    case Page::Games: Games(f); break;
    case Page::Background: Background(f); break;
    case Page::Logs: Logs(f); break;
    case Page::Settings: SettingsPage(f); break;
    }
    return f.y;
}

void Window::Navigate(Page page) {
    page_ = page;
    scroll_[page] = 0;
    if (page == Page::Logs) logLines_.clear();          // reread on open
    Invalidate();
}

// ------------------------------------------------------------------ dialogs, toasts and tooltip

void Window::Confirm(std::wstring title, std::wstring message, std::wstring confirm, std::function<void()> action, bool destructive) {
    sheet_ = Sheet{ std::move(title), std::move(message), std::move(confirm), std::move(action), destructive };
    ui_.SetFocus(L"");
    Invalidate();
}

// nav background, a wire edge, radius 12, padding 24; no shadow
static D2D1_RECT_F DialogCard(Canvas& canvas, float width, float height, float w, float h) {
    canvas.Fill(Rect(0, 0, width, height), color::Overlay);
    D2D1_RECT_F card = Rect((width - w) / 2, (height - h) / 2, w, h);
    canvas.Fill(card, color::Nav, RadiusMd);
    canvas.Stroke(card, color::Wire, RadiusMd);
    return card;
}

void Window::DrawSheet(float width, float height) {
    auto& s = *sheet_;
    ui_.Interact(L"sheet/shade", Rect(0, 0, width, height), [this] { sheet_.reset(); });
    const float w = 440, pad = Space24;
    auto message = Wrapped(Type::Body);
    float messageHeight = canvas_.Measure(s.message, w - pad * 2, message).height;
    float titleHeight = Spec(Type::Heading).line;
    float h = pad + titleHeight + Space8 + messageHeight + Space24 + ControlMd + pad;
    D2D1_RECT_F card = DialogCard(canvas_, width, height, w, h);
    ui_.Interact(L"sheet/card", card, nullptr);
    canvas_.Text(s.title, Rect(card.left + pad, card.top + pad, w - pad * 2, titleHeight), color::Text, Style(Type::Heading));
    canvas_.Text(s.message, Rect(card.left + pad, card.top + pad + titleHeight + Space8, w - pad * 2, messageHeight + 2), color::TextMuted, message);

    // a destructive confirmation is an outline button with its verb, so nothing in the system turns red
    ButtonSpec ok{ s.confirm, nullptr, s.destructive ? ButtonStyle::Outline : ButtonStyle::Primary, [this] { auto a = sheet_->action; sheet_.reset(); if (a) a(); } };
    ButtonSpec cancel{ L"Cancel", nullptr, s.destructive ? ButtonStyle::Primary : ButtonStyle::Outline, [this] { sheet_.reset(); } };
    float okW = ui_.ButtonWidth(ok), cancelW = ui_.ButtonWidth(cancel);
    float by = card.bottom - pad - ControlMd;
    ui_.Button(Rect(card.right - pad - okW - Space12 - cancelW, by, cancelW, ControlMd), cancel, L"sheet/cancel");
    ui_.Button(Rect(card.right - pad - okW, by, okW, ControlMd), ok, L"sheet/ok");
}

// The apps a person can see right now: visible, unowned top-level windows with a title, outside the Windows folder.
static std::vector<std::pair<std::wstring, std::wstring>> RunningApps() {
    struct Search { std::vector<std::pair<std::wstring, std::wstring>> apps; std::wstring windows; DWORD self; } search;
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    search.windows = Lower(windows) + L"\\";
    search.self = GetCurrentProcessId();
    EnumWindows([](HWND hwnd, LPARAM param) -> BOOL {
        auto s = reinterpret_cast<Search*>(param);
        if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) || (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) || !GetWindowTextLengthW(hwnd)) return TRUE;
        BOOL cloaked = FALSE;
        if (SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return TRUE;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        if (pid == s->self) return TRUE;
        auto exe = ImagePath(pid);
        if (exe.empty() || Lower(exe).starts_with(s->windows)) return TRUE;
        for (auto& [name, path] : s->apps) if (_wcsicmp(path.c_str(), exe.c_str()) == 0) return TRUE;
        s->apps.push_back({ FriendlyName(exe), exe });
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    std::sort(search.apps.begin(), search.apps.end(), [](auto& a, auto& b) { return _wcsicmp(a.first.c_str(), b.first.c_str()) < 0; });
    return search.apps;
}

void Window::Pick(std::wstring title, std::function<void(const std::wstring& exe)> chosen) {
    Picker picker{ std::move(title), {}, std::move(chosen) };
    for (auto& [name, exe] : RunningApps()) picker.items.push_back({ name, exe });
    picker_ = std::move(picker);
    ui_.SetFocus(L"");
    Invalidate();
}

void Window::DrawPicker(float width, float height) {
    auto& k = *picker_;
    ui_.Interact(L"sheet/shade", Rect(0, 0, width, height), [this] { picker_.reset(); });
    const float w = 480, pad = Space24, row = 56;
    float titleHeight = Spec(Type::Heading).line, subHeight = Spec(Type::BodySm).line;
    float head = pad + titleHeight + Space4 + subHeight + Space16, foot = Space16 + ControlMd + pad;
    float listHeight = std::max(row, k.items.size() * row);
    float h = std::min(height - Space48 * 2, head + listHeight + foot);
    D2D1_RECT_F card = DialogCard(canvas_, width, height, w, h);
    ui_.Interact(L"sheet/card", card, nullptr);
    canvas_.Text(k.title, Rect(card.left + pad, card.top + pad, w - pad * 2, titleHeight), color::Text, Style(Type::Heading));
    canvas_.Text(L"Apps running with a window. Choose a file for one that is not running.", Rect(card.left + pad, card.top + pad + titleHeight + Space4, w - pad * 2, subHeight), color::TextMuted, Style(Type::BodySm));

    D2D1_RECT_F list{ card.left + Space12, card.top + head, card.right - Space12, card.bottom - foot };
    k.viewHeight = Height(list);
    k.contentHeight = listHeight;
    k.scroll = std::clamp(k.scroll, 0.f, std::max(0.f, k.contentHeight - k.viewHeight));
    canvas_.Fill(Rect(card.left + 1, list.top - 1, w - 2, 1), color::Hairline);
    canvas_.Fill(Rect(card.left + 1, list.bottom, w - 2, 1), color::Hairline);
    canvas_.PushClip(list);
    canvas_.Translate(0, -k.scroll);
    ui_.SetOffset(0, -k.scroll, list);
    if (k.items.empty())
        canvas_.Text(L"No apps with a window are running. Choose a file instead.", Rect(list.left + Space12, list.top, Width(list) - Space24, row), color::TextMuted, Style(Type::BodySm));
    for (size_t i = 0; i < k.items.size(); ++i) {
        auto item = k.items[i];
        D2D1_RECT_F r = Rect(list.left, list.top + i * row, Width(list), row);
        bool hover = ui_.Interact(L"sheet/pick/" + std::to_wstring(i), r, [this, item] { auto chosen = picker_->chosen; picker_.reset(); if (chosen) chosen(item.exe); }, true, RadiusSm);
        if (hover) canvas_.Fill(r, color::Hover, RadiusSm);
        canvas_.Text(item.name, Rect(r.left + Space12, r.top + Space8, Width(r) - Space24, 20), color::Text, Style(Type::BodySm));
        canvas_.Text(item.exe, Rect(r.left + Space12, r.top + Space8 + 22, Width(r) - Space24, 20), color::TextMuted, Style(Type::Code));
    }
    canvas_.ResetTransform();
    ui_.ClearOffset();
    canvas_.PopClip();

    ButtonSpec browse{ L"Choose file", icons::Folder, ButtonStyle::Outline, [this] {
        auto title = picker_->title;
        auto chosen = picker_->chosen;
        picker_.reset();
        auto exe = BrowseExe(title.c_str());
        if (!exe.empty() && chosen) chosen(exe);
    } };
    ButtonSpec cancel{ L"Cancel", nullptr, ButtonStyle::Outline, [this] { picker_.reset(); } };
    float browseW = ui_.ButtonWidth(browse), cancelW = ui_.ButtonWidth(cancel);
    float by = card.bottom - pad - ControlMd;
    ui_.Button(Rect(card.left + pad, by, browseW, ControlMd), browse, L"sheet/browse");
    ui_.Button(Rect(card.right - pad - cancelW, by, cancelW, ControlMd), cancel, L"sheet/cancel");
}

void Window::Toast(const std::wstring& text, Tone tone) {
    toasts_.push_back({ text, tone, GetTickCount64() + 4000 });
    Invalidate();
}

// nav background, a wire edge, radius 12, bottom right 24 from the edges; the icon says what kind of message it is
void Window::DrawToasts(float width, float height) {
    auto now = GetTickCount64();
    std::erase_if(toasts_, [now](auto& t) { return t.until <= now; });
    float y = height - Space24;
    auto style = Style(Type::BodySm);
    for (auto it = toasts_.rbegin(); it != toasts_.rend(); ++it) {
        auto size = canvas_.Measure(it->text, 420, style);
        float w = Space16 + IconSm + Space12 + size.width + Space16, h = 48;
        D2D1_RECT_F r = Rect(width - Space24 - w, y - h, w, h);
        float alpha = std::min(1.f, (it->until - now) / 120.f);
        canvas_.Fill(r, WithAlpha(color::Nav, alpha), RadiusMd);
        canvas_.Stroke(r, WithAlpha(color::Wire, alpha), RadiusMd);
        const char* icon = it->tone == Tone::Alert ? icons::Alert : it->tone == Tone::On ? icons::Check : icons::Info;
        canvas_.Icon(icon, { r.left + Space16, r.top + (h - IconSm) / 2 }, IconSm, WithAlpha(color::Text, alpha));
        canvas_.Text(it->text, Rect(r.left + Space16 + IconSm + Space12, r.top, size.width + 2, h), WithAlpha(color::Text, alpha), style);
        y -= h + Space8;
    }
}

// white with a black label, radius 4, after 400 ms
void Window::DrawTooltip(float width, float height) {
    if (tipId_.empty() || Modal() || GetTickCount64() - tipSince_ < kTooltipDelayMs) return;
    const Hit* hit = nullptr;
    for (auto& h : ui_.Hits()) if (h.id == tipId_) hit = &h;
    if (!hit || hit->tooltip.empty()) return;
    auto size = canvas_.Measure(hit->tooltip, 300, Style(Type::Caption));
    float w = size.width + Space16, h = size.height + Space8;
    float x = std::clamp(mouse_.x - w / 2, 4.f, width - w - 4), y = hit->rect.bottom + Space8;
    if (y + h > height - 4) y = hit->rect.top - h - Space8;
    D2D1_RECT_F r = Rect(x, y, w, h);
    canvas_.Fill(r, color::Text, RadiusSm);
    canvas_.Text(hit->tooltip, r, color::Deep, Style(Type::Caption, Align::Center));
}

// ------------------------------------------------------------------ --render

bool Window::RenderToFile(Page page, const std::wstring& path) {
    if (!canvas_.Factory() && !canvas_.Init()) return false;
    const UINT w = 1120, h = 760;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1RenderTarget> target;
    if (FAILED(canvas_.Wic()->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) return false;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (FAILED(canvas_.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf()))) return false;

    rendering_ = true;
    page_ = page;
    canvas_.ReleaseDeviceResources();
    for (int pass = 0; pass < 2; ++pass) {             // the first pass measures the page, the second draws it settled
        target->BeginDraw();
        canvas_.Begin(target.Get(), 1.f);
        Draw(static_cast<float>(w), static_cast<float>(h));
        canvas_.End();
        target->EndDraw();
    }
    canvas_.ReleaseDeviceResources();
    rendering_ = false;

    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(canvas_.Wic()->CreateStream(stream.GetAddressOf())) || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
        FAILED(canvas_.Wic()->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf())) || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), nullptr)) || FAILED(frame->Initialize(nullptr)) || FAILED(frame->WriteSource(bitmap.Get(), nullptr)) ||
        FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return false;
    return true;
}

}  // namespace xww::ui
