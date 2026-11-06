#include "ui/widgets.h"

#include <algorithm>
#include <cmath>

namespace xww::ui {

using namespace metric;
namespace col = color;

void Ui::Begin(const Theme& theme, D2D1_POINT_2F mouse, bool hasMouse, ULONGLONG now) {
    t = &theme;
    mouse_ = mouse;
    hasMouse_ = hasMouse;
    last_ = now_ ? now_ : now;
    now_ = now;
    animating_ = false;
    hits_.clear();
    hot_ = hotNext_;
    hotNext_.clear();
    focusDrawn_ = false;
    ClearOffset();
}

bool Ui::Interact(const std::wstring& id, const D2D1_RECT_F& r, std::function<void()> action, bool enabled, float radius, std::wstring tooltip) {
    D2D1_RECT_F screen{ std::max(r.left + dx_, clip_.left), std::max(r.top + dy_, clip_.top), std::min(r.right + dx_, clip_.right), std::min(r.bottom + dy_, clip_.bottom) };
    if (screen.right <= screen.left || screen.bottom <= screen.top || !enabled) return false;
    hits_.push_back({ screen, id, std::move(action), std::move(tooltip), radius });
    if (!focus_.empty() && id == focus_) { focusRect_ = { r.left + dx_, r.top + dy_, r.right + dx_, r.bottom + dy_ }; focusRadius_ = radius; focusDrawn_ = true; }
    bool hover = hasMouse_ && Inside(screen, mouse_);
    if (hover) hotNext_ = id;
    return hover;
}

// 2 px `text` outline with a 2 px offset
void Ui::DrawFocusRing() {
    if (focusDrawn_) c.Stroke(Inflate(focusRect_, 4), col::Text, focusRadius_ > 0 ? focusRadius_ + 4 : 0, 2);
}

float Ui::Ease(const std::wstring& id, float target, float seconds) {
    auto it = anim_.find(id);
    if (it == anim_.end() || t->reduceMotion) { anim_[id] = target; return target; }
    float dt = static_cast<float>(now_ - last_) / 1000.f;
    float& v = it->second;
    float k = 1.f - std::exp(-3.f * dt / seconds);      // covers ~95% of the distance in `seconds`
    v += (target - v) * k;
    if (std::fabs(target - v) < 0.002f) v = target;
    else animating_ = true;
    return v;
}

// ------------------------------------------------------------------ buttons

float Ui::ButtonWidth(const ButtonSpec& b, ButtonSize size) {
    float text = b.label.empty() ? 0 : c.Measure(b.label, 1000, Style(Type::BodySm)).width;
    float icon = b.icon ? IconSm + (b.label.empty() ? 0 : Space8) : 0;
    float pad = b.style == ButtonStyle::Text ? Space8 : size == ButtonSize::Small ? Space16 : Space20;
    return std::ceil(text + icon + pad * 2);
}

void Ui::Button(const D2D1_RECT_F& r, const ButtonSpec& b, const std::wstring& id, ButtonSize) {
    float radius = Height(r) / 2;
    bool hover = Interact(id, r, b.action, b.enabled, radius);
    float opacity = b.enabled ? 1.f : 0.4f;
    D2D1_COLOR_F fg = col::Text;
    switch (b.style) {
    case ButtonStyle::Primary:          // stays the same on hover and press: it is the anchor
        c.CtaShadow(r);
        c.Fill(r, WithAlpha(col::Text, opacity), radius);
        fg = col::Deep;
        break;
    case ButtonStyle::Outline:
        if (hover) c.Fill(r, col::Hover, radius);
        c.Stroke(r, WithAlpha(col::Wire, opacity), radius);
        break;
    case ButtonStyle::Text:
        fg = hover ? col::Text : col::TextMuted;
        break;
    }
    float text = b.label.empty() ? 0 : c.Measure(b.label, 1000, Style(Type::BodySm)).width;
    float content = (b.icon ? IconSm : 0) + (b.icon && !b.label.empty() ? Space8 : 0) + text;
    float x = r.left + (Width(r) - content) / 2;
    auto color = WithAlpha(fg, opacity);
    if (b.icon) { c.Icon(b.icon, { x, r.top + (Height(r) - IconSm) / 2 }, IconSm, color); x += IconSm + (b.label.empty() ? 0 : Space8); }
    if (!b.label.empty()) c.Text(b.label, { x, r.top, x + text + 2, r.bottom }, color, Style(Type::BodySm));
}

void Ui::CaptionButton(const D2D1_RECT_F& r, const char* icon, std::function<void()> action, const std::wstring& id, const std::wstring& tooltip) {
    bool hover = Interact(id, r, std::move(action), true, 0, tooltip);
    if (hover) c.Fill(r, col::Hover);
    c.Icon(icon, { r.left + (Width(r) - IconSm) / 2, r.top + (Height(r) - IconSm) / 2 }, IconSm, hover ? col::Text : col::TextMuted);
}

// ------------------------------------------------------------------ switch

// 36 x 20 pill. Off: recess with a wire edge and a muted knob. On: a white track with a black knob.
void Ui::Switch(const D2D1_RECT_F& r, bool on, const std::function<void(bool)>& f, const std::wstring& id, bool enabled) {
    Interact(id, r, [f, on] { if (f) f(!on); }, enabled, Height(r) / 2);
    float k = Ease(id, on ? 1.f : 0.f, motion::Fast);
    float alpha = enabled ? 1.f : 0.4f;
    float radius = Height(r) / 2;
    if (k < 1) { c.Fill(r, WithAlpha(col::Recess, alpha), radius); c.Stroke(r, WithAlpha(col::Wire, alpha), radius); }
    if (k > 0) c.Fill(r, WithAlpha(col::Text, alpha * k), radius);
    float knob = Height(r) - 8;
    float x = r.left + 4 + (Width(r) - knob - 8) * k;
    D2D1_COLOR_F knobColor{ col::TextMuted.r + (col::Deep.r - col::TextMuted.r) * k, col::TextMuted.g + (col::Deep.g - col::TextMuted.g) * k, col::TextMuted.b + (col::Deep.b - col::TextMuted.b) * k, alpha };
    c.Circle({ x + knob / 2, r.top + Height(r) / 2 }, knob / 2, knobColor);
}

// ------------------------------------------------------------------ segmented

static float SegmentWidth(Canvas& c, const std::wstring& option) { return std::max(64.f, c.Measure(option, 1000, Style(Type::BodySm)).width + Space32); }

float Ui::SegmentedWidth(const std::vector<std::wstring>& options) {
    float w = Space8;
    for (auto& o : options) w += SegmentWidth(c, o);
    return std::ceil(w);
}

// Pill track in recess with a wire edge; the active segment is a white pill with a black label.
void Ui::Segmented(const D2D1_RECT_F& r, const std::vector<std::wstring>& options, int selected, const std::function<void(int)>& f, const std::wstring& id) {
    float radius = Height(r) / 2;
    c.Fill(r, col::Recess, radius);
    c.Stroke(r, col::Wire, radius);
    std::vector<D2D1_RECT_F> items;
    float x = r.left + Space4;
    for (auto& o : options) { float w = SegmentWidth(c, o); items.push_back({ x, r.top + Space4, x + w, r.bottom - Space4 }); x += w; }
    float inner = Height(items.empty() ? r : items[0]) / 2;
    if (selected >= 0 && selected < static_cast<int>(items.size())) {
        float left = Ease(id + L"/left", items[selected].left, motion::Fast), right = Ease(id + L"/right", items[selected].right, motion::Fast);
        c.Fill({ left, r.top + Space4, right, r.bottom - Space4 }, col::Text, inner);
    }
    for (int i = 0; i < static_cast<int>(options.size()); ++i) {
        bool hover = Interact(id + L"/" + std::to_wstring(i), items[i], [f, i] { if (f) f(i); }, true, inner);
        if (hover && i != selected) c.Fill(items[i], col::Hover, inner);
        c.Text(options[i], items[i], i == selected ? col::Deep : hover ? col::Text : col::TextMuted, Style(Type::BodySm, Align::Center));
    }
}

// ------------------------------------------------------------------ badge and tag

float Ui::BadgeWidth(const BadgeSpec& b) { return std::ceil(c.Measure(b.text, 1000, Style(Type::Caption)).width + Space8 * 2); }

void Ui::Badge(D2D1_POINT_2F at, const BadgeSpec& b) {
    D2D1_RECT_F r = Rect(at.x, at.y, BadgeWidth(b), 22);
    if (b.tag) {
        c.Stroke(r, col::Wire, RadiusSm);
        c.Text(b.text, r, col::TextMuted, Style(Type::Caption, Align::Center));
    } else {
        c.Fill(r, col::Recess, Height(r) / 2);
        c.Stroke(r, col::Wire, Height(r) / 2);
        c.Text(b.text, r, col::Text, Style(Type::Caption, Align::Center));
    }
}

// ------------------------------------------------------------------ navigation

// Current: white text and icon with a white indicator; the rest muted.
void Ui::Nav(const D2D1_RECT_F& r, const char* icon, const std::wstring& label, bool selected, std::function<void()> action, const std::wstring& id) {
    bool hover = Interact(id, r, std::move(action), true, RadiusSm);
    if (selected || hover) c.Fill(r, col::Hover, RadiusSm);
    if (selected) c.Fill(Rect(r.left, r.top + Space12, 2, Height(r) - Space24), col::Text);
    auto fg = selected || hover ? col::Text : col::TextMuted;
    c.Icon(icon, { r.left + Space12, r.top + (Height(r) - IconSm) / 2 }, IconSm, fg);
    c.Text(label, { r.left + Space12 + IconSm + Space12, r.top, r.right - Space8, r.bottom }, fg, Style(Type::BodySm));
}

// ------------------------------------------------------------------ blocks

void Ui::Header(Flow& f, const std::wstring& title, const std::wstring& subtitle, const std::vector<ButtonSpec>& actions) {
    float buttons = 0;
    for (auto& a : actions) buttons += ButtonWidth(a) + Space12;
    float textWidth = std::min(TextWidth, f.width - buttons - (buttons > 0 ? Space16 : 0));
    float titleHeight = Spec(Type::HeadingLg).line;
    c.Text(title, Rect(f.x, f.y, f.width - buttons, titleHeight), col::Text, Style(Type::HeadingLg));
    auto sub = Wrapped(Type::Body);
    float subHeight = subtitle.empty() ? 0 : c.Measure(subtitle, textWidth, sub).height;
    if (!subtitle.empty()) c.Text(subtitle, Rect(f.x, f.y + titleHeight + Space8, textWidth, subHeight + 2), col::TextMuted, sub);
    float x = f.x + f.width;
    for (size_t i = actions.size(); i-- > 0;) {
        float w = ButtonWidth(actions[i]);
        x -= w;
        Button(Rect(x, f.y + (titleHeight - ControlMd) / 2, w, ControlMd), actions[i], L"header/" + actions[i].label);
        x -= Space12;
    }
    f.y += titleHeight + (subHeight > 0 ? Space8 + subHeight : 0) + Space40;
}

float Ui::ControlWidth(const Control& control) {
    switch (control.kind) {
    case Control::Kind::Toggle: return SwitchWidth;
    case Control::Kind::Segmented: return SegmentedWidth(control.options);
    case Control::Kind::Text: return std::min(320.f, c.Measure(control.text, 1000, Style(Type::BodySm)).width + 2);
    case Control::Kind::Buttons: {
        float w = 0;
        for (auto& b : control.buttons) w += ButtonWidth(b, ButtonSize::Small) + Space8;
        return w > 0 ? w - Space8 : 0;
    }
    default: return 0;
    }
}

static float TextLeft(const RowSpec& row) { return Space24 + (row.icon ? IconSm + Space12 : 0); }
static TextStyle DescriptionStyle(const RowSpec& row) { return Wrapped(row.monoDescription ? Type::Code : Type::Caption); }

float Ui::RowHeight(const RowSpec& row, float width) {
    float control = ControlWidth(row.control);
    float textWidth = width - TextLeft(row) - Space24 - (control > 0 ? control + Space16 : 0);
    float h = Spec(Type::BodySm).line;
    if (!row.description.empty()) h += 2 + c.Measure(row.description, textWidth, DescriptionStyle(row)).height;
    float controlHeight = row.control.kind == Control::Kind::Toggle ? SwitchHeight : row.control.kind == Control::Kind::None || row.control.kind == Control::Kind::Text ? 0 : ControlSm;
    return std::ceil(std::max({ h + Space12 * 2, RowMin, controlHeight + Space12 * 2 }));
}

void Ui::Row(const D2D1_RECT_F& r, const RowSpec& row) {
    float control = ControlWidth(row.control);
    float left = r.left + TextLeft(row);
    float textWidth = r.right - Space24 - (control > 0 ? control + Space16 : 0) - left;
    auto descStyle = DescriptionStyle(row);
    float line = Spec(Type::BodySm).line;
    float descHeight = row.description.empty() ? 0 : c.Measure(row.description, textWidth, descStyle).height;
    float block = line + (descHeight > 0 ? 2 + descHeight : 0);
    float top = r.top + (Height(r) - block) / 2;

    if (row.icon) c.Icon(row.icon, { r.left + Space24, top + (line - IconSm) / 2 }, IconSm, col::TextMuted);
    float titleWidth = std::min(c.Measure(row.title, textWidth, Style(Type::BodySm)).width + 1, textWidth);
    c.Text(row.title, Rect(left, top, titleWidth, line), col::Text, Style(Type::BodySm));
    float bx = left + titleWidth + Space8;
    for (auto& badge : row.badges) {
        float w = BadgeWidth(badge);
        if (bx + w > left + textWidth) break;
        Badge({ bx, top - 1 }, badge);
        bx += w + Space8;
    }
    if (descHeight > 0) c.Text(row.description, Rect(left, top + line + 2, textWidth, descHeight + 2), col::TextMuted, descStyle);

    float cx = r.right - Space24 - control, cy = r.top + Height(r) / 2;
    switch (row.control.kind) {
    case Control::Kind::Toggle:
        Switch(Rect(cx, cy - SwitchHeight / 2, SwitchWidth, SwitchHeight), row.control.on, row.control.toggled, row.control.id, row.control.enabled);
        break;
    case Control::Kind::Segmented:
        Segmented(Rect(cx, cy - ControlSm / 2, control, ControlSm), row.control.options, row.control.selected, row.control.select, row.control.id);
        break;
    case Control::Kind::Text:
        c.Text(row.control.text, Rect(cx, cy - line / 2, control, line), col::TextMuted, Style(Type::BodySm, Align::Trailing));
        break;
    case Control::Kind::Buttons:
        for (auto& b : row.control.buttons) {
            float w = ButtonWidth(b, ButtonSize::Small);
            Button(Rect(cx, cy - ControlSm / 2, w, ControlSm), b, row.title + L"/" + b.label + (b.icon ? L"/i" : L""), ButtonSize::Small);
            cx += w + Space8;
        }
        break;
    default: break;
    }
}

// A list in a card: black like the page, separated from it only by a hairline.
void Ui::Group(Flow& f, const std::wstring& title, const std::vector<RowSpec>& rows, const std::wstring& footer) {
    if (!title.empty()) {
        c.Text(title, Rect(f.x, f.y, f.width, Spec(Type::Caption).line), col::TextMuted, Style(Type::Caption));
        f.y += Spec(Type::Caption).line + Space8;
    }
    std::vector<float> heights;
    float total = 0;
    for (auto& row : rows) { heights.push_back(RowHeight(row, f.width)); total += heights.back(); }
    D2D1_RECT_F card = Rect(f.x, f.y, f.width, total);
    c.Fill(card, col::Canvas, RadiusMd);
    float y = f.y;
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i) c.Fill(Rect(f.x + 1, y, f.width - 2, 1), col::Hairline);
        Row(Rect(f.x, y, f.width, heights[i]), rows[i]);
        y += heights[i];
    }
    c.Stroke(card, col::Hairline, RadiusMd);
    f.y = card.bottom;
    if (!footer.empty()) {
        auto style = Wrapped(Type::Caption);
        float h = c.Measure(footer, std::min(TextWidth, f.width), style).height;
        c.Text(footer, Rect(f.x, f.y + Space8, std::min(TextWidth, f.width), h + 2), col::TextMuted, style);
        f.y += Space8 + h;
    }
    f.y += Space32;
}

}  // namespace xww::ui
