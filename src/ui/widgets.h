//  Immediate-mode controls of the design system: pill buttons (primary,
//  outline, text), switches, segmented controls, badges and tags, sidebar
//  links and lists in hairline cards. Every frame redraws everything and
//  registers hit areas; hover, press, keyboard focus and animations carry
//  over between frames by id.
#pragma once

#include "ui/canvas.h"

#include <functional>
#include <vector>

namespace xww::ui {

enum class ButtonStyle { Primary, Outline, Text };
enum class ButtonSize { Small, Medium };

struct ButtonSpec {
    std::wstring label;
    const char* icon = nullptr;
    ButtonStyle style = ButtonStyle::Outline;
    std::function<void()> action;
    bool enabled = true;
};

// A badge states something (`Running`, `On`); a tag annotates (`Default`, `Opt-in`). Neither carries color.
struct BadgeSpec { std::wstring text; bool tag = false; };

struct Control {
    enum class Kind { None, Toggle, Buttons, Segmented, Text } kind = Kind::None;
    std::wstring id;
    bool enabled = true;
    bool on = false;                                // Toggle
    std::function<void(bool)> toggled;
    std::vector<ButtonSpec> buttons;                // Buttons
    std::vector<std::wstring> options;              // Segmented
    int selected = -1;
    std::function<void(int)> select;
    std::wstring text;                              // Text

    static Control Switch(std::wstring id, bool on, std::function<void(bool)> f, bool enabled = true) { Control c; c.kind = Kind::Toggle; c.id = std::move(id); c.on = on; c.toggled = std::move(f); c.enabled = enabled; return c; }
    static Control Button(ButtonSpec b) { Control c; c.kind = Kind::Buttons; c.buttons = { std::move(b) }; return c; }
    static Control Buttons(std::vector<ButtonSpec> b) { Control c; c.kind = Kind::Buttons; c.buttons = std::move(b); return c; }
    static Control Segments(std::wstring id, std::vector<std::wstring> o, int sel, std::function<void(int)> f) { Control c; c.kind = Kind::Segmented; c.id = std::move(id); c.options = std::move(o); c.selected = sel; c.select = std::move(f); return c; }
    static Control Label(std::wstring t) { Control c; c.kind = Kind::Text; c.text = std::move(t); return c; }
};

struct RowSpec {
    std::wstring title, description;
    std::vector<BadgeSpec> badges;
    Control control;
    bool monoDescription = false;
    const char* icon = nullptr;                     // leading, text-muted
};

struct Flow { float x, y, width; };                 // a column being filled top to bottom

struct Hit {
    D2D1_RECT_F rect;
    std::wstring id;
    std::function<void()> action;
    std::wstring tooltip;
    float radius = 0;
};

class Ui {
public:
    explicit Ui(Canvas& canvas) : c(canvas) {}

    // frame
    void Begin(const Theme& theme, D2D1_POINT_2F mouse, bool hasMouse, ULONGLONG now);
    bool Animating() const { return animating_; }
    const std::vector<Hit>& Hits() const { return hits_; }
    void SetPressed(const std::wstring& id) { pressed_ = id; }
    void SetHasMouse(bool on) { hasMouse_ = on; }      // off while a dialog covers the page
    void SetFocus(const std::wstring& id) { focus_ = id; }
    const std::wstring& Focus() const { return focus_; }
    const std::wstring& Hot() const { return hot_; }
    void SetOffset(float dx, float dy, const D2D1_RECT_F& clip) { dx_ = dx; dy_ = dy; clip_ = clip; }
    void ClearOffset() { dx_ = dy_ = 0; clip_ = { -1e6f, -1e6f, 1e6f, 1e6f }; }
    void DrawFocusRing();                               // after everything else, in window coordinates

    // primitives
    bool Interact(const std::wstring& id, const D2D1_RECT_F& r, std::function<void()> action, bool enabled = true, float radius = 0, std::wstring tooltip = {});
    float Ease(const std::wstring& id, float target, float seconds);

    // controls
    float ButtonWidth(const ButtonSpec& b, ButtonSize size = ButtonSize::Medium);
    void Button(const D2D1_RECT_F& r, const ButtonSpec& b, const std::wstring& id, ButtonSize size = ButtonSize::Medium);
    void Switch(const D2D1_RECT_F& r, bool on, const std::function<void(bool)>& f, const std::wstring& id, bool enabled);
    float SegmentedWidth(const std::vector<std::wstring>& options);
    void Segmented(const D2D1_RECT_F& r, const std::vector<std::wstring>& options, int selected, const std::function<void(int)>& f, const std::wstring& id);
    float BadgeWidth(const BadgeSpec& b);
    void Badge(D2D1_POINT_2F at, const BadgeSpec& b);
    void Nav(const D2D1_RECT_F& r, const char* icon, const std::wstring& label, bool selected, std::function<void()> action, const std::wstring& id);
    void CaptionButton(const D2D1_RECT_F& r, const char* icon, std::function<void()> action, const std::wstring& id, const std::wstring& tooltip);

    // layout blocks (advance the flow)
    void Header(Flow& f, const std::wstring& title, const std::wstring& subtitle, const std::vector<ButtonSpec>& actions = {});
    void Group(Flow& f, const std::wstring& title, const std::vector<RowSpec>& rows, const std::wstring& footer = L"");
    float ControlWidth(const Control& control);
    float RowHeight(const RowSpec& row, float width);
    void Row(const D2D1_RECT_F& r, const RowSpec& row);

    Canvas& c;
    const Theme* t = nullptr;

private:
    std::vector<Hit> hits_;
    std::wstring hot_, pressed_, hotNext_, focus_;
    D2D1_RECT_F focusRect_{};
    float focusRadius_ = 0;
    bool focusDrawn_ = false;
    D2D1_POINT_2F mouse_{ -1, -1 };
    bool hasMouse_ = false, animating_ = false;
    float dx_ = 0, dy_ = 0;
    D2D1_RECT_F clip_{ -1e6f, -1e6f, 1e6f, 1e6f };
    ULONGLONG now_ = 0, last_ = 0;
    std::map<std::wstring, float> anim_;
};

}  // namespace xww::ui
