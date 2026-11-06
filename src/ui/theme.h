//  Design tokens of the XWWRAW design system: one dark, achromatic theme
//  on a true black canvas, the type scale, the spacing scale, three radii,
//  control sizes and motion. There is no light theme and no accent color.
#pragma once

#include <d2d1.h>
#include <cstdint>

namespace xww::ui {

constexpr D2D1_COLOR_F Hex(uint32_t rgb, float alpha = 1.f) {
    return { ((rgb >> 16) & 0xFF) / 255.f, ((rgb >> 8) & 0xFF) / 255.f, (rgb & 0xFF) / 255.f, alpha };
}

constexpr D2D1_COLOR_F WithAlpha(D2D1_COLOR_F c, float alpha) { return { c.r, c.g, c.b, c.a * alpha }; }

namespace color {
inline constexpr D2D1_COLOR_F Canvas = Hex(0x000000);      // page and window background
inline constexpr D2D1_COLOR_F Nav = Hex(0x0E0E10);         // title bar, sidebar, dialogs, menus, toasts
inline constexpr D2D1_COLOR_F Recess = Hex(0x181818);      // fields, code blocks, switch and segmented tracks
inline constexpr D2D1_COLOR_F Deep = Hex(0x000000);        // label on the white primary button
inline constexpr D2D1_COLOR_F Text = Hex(0xFAFAFA);
inline constexpr D2D1_COLOR_F TextSoft = Hex(0xEBECED);
inline constexpr D2D1_COLOR_F TextMuted = Hex(0xA1A1AA);
inline constexpr D2D1_COLOR_F TextSubtle = Hex(0x696970);  // non-essential metadata only
inline constexpr D2D1_COLOR_F Smoke = Hex(0x454546);       // disabled
inline constexpr D2D1_COLOR_F Wire = Hex(0x5C5C61);        // control and field borders
inline constexpr D2D1_COLOR_F Hairline = Hex(0x232324);    // card borders and row dividers
inline constexpr D2D1_COLOR_F Hover = Hex(0xFFFFFF, 0.05f);
inline constexpr D2D1_COLOR_F Overlay = Hex(0x0E0E10, 0.80f);
}

struct Theme {
    bool reduceMotion = false;
};

// Status is told with an icon and words, never with color.
enum class Tone { Neutral, On, Paused, Off, Alert };

// size / line height in DIPs, weight, tracking in em, Geist Mono or Geist
enum class Type { Caption, BodySm, Body, Subheading, Heading, HeadingLg, Code, Display, DisplayXl, Mark };

namespace metric {
inline constexpr float Space4 = 4, Space8 = 8, Space12 = 12, Space16 = 16, Space20 = 20, Space24 = 24, Space32 = 32, Space40 = 40, Space48 = 48, Space64 = 64;
inline constexpr float RadiusSm = 4, RadiusMd = 12;
inline constexpr float ControlSm = 32, ControlMd = 40, RowMin = 44;
inline constexpr float IconSm = 16, IconMd = 20, IconStroke = 1.5f;
inline constexpr float SwitchWidth = 36, SwitchHeight = 20;
inline constexpr float SidebarWidth = 240, TitleBarHeight = 56, PageWidth = 1200, PageGutter = 40, TextWidth = 640;
inline constexpr float CaptionButtonWidth = 46, CaptionButtonHeight = 32;
}

namespace motion {
inline constexpr float Fast = 0.12f, Base = 0.20f, Slow = 0.32f;
}

}  // namespace xww::ui
