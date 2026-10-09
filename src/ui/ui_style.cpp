// UI styles: built-in themes (color palettes) and looks (widget shapes and effects), DPI scaling, and the text theme
// format.
#include "ui_internal.h"

#include "../text_layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace drizzy {
using namespace ui_detail;

namespace {

// Indexed by UiStyleVar; the names double as theme file keys. Names are encrypted (detail::ObfEntry).
const VarInfo kVarInfo[] = {
    {DZ_OBF("alpha"), 1, offsetof(UiStyle, alpha)},
    {DZ_OBF("windowPadding"), 2, offsetof(UiStyle, windowPadding)},
    {DZ_OBF("windowRounding"), 1, offsetof(UiStyle, windowRounding)},
    {DZ_OBF("windowBorderSize"), 1, offsetof(UiStyle, windowBorderSize)},
    {DZ_OBF("windowShadowSize"), 1, offsetof(UiStyle, windowShadowSize)},
    {DZ_OBF("windowMinSize"), 2, offsetof(UiStyle, windowMinSize)},
    {DZ_OBF("framePadding"), 2, offsetof(UiStyle, framePadding)},
    {DZ_OBF("frameRounding"), 1, offsetof(UiStyle, frameRounding)},
    {DZ_OBF("frameBorderSize"), 1, offsetof(UiStyle, frameBorderSize)},
    {DZ_OBF("itemSpacing"), 2, offsetof(UiStyle, itemSpacing)},
    {DZ_OBF("itemInnerSpacing"), 2, offsetof(UiStyle, itemInnerSpacing)},
    {DZ_OBF("indentSpacing"), 1, offsetof(UiStyle, indentSpacing)},
    {DZ_OBF("scrollbarSize"), 1, offsetof(UiStyle, scrollbarSize)},
    {DZ_OBF("scrollbarRounding"), 1, offsetof(UiStyle, scrollbarRounding)},
    {DZ_OBF("grabMinSize"), 1, offsetof(UiStyle, grabMinSize)},
    {DZ_OBF("grabRounding"), 1, offsetof(UiStyle, grabRounding)},
    {DZ_OBF("popupRounding"), 1, offsetof(UiStyle, popupRounding)},
    {DZ_OBF("tabRounding"), 1, offsetof(UiStyle, tabRounding)},
    {DZ_OBF("buttonTextAlign"), 2, offsetof(UiStyle, buttonTextAlign)},
    {DZ_OBF("cellPadding"), 2, offsetof(UiStyle, cellPadding)},
    {DZ_OBF("glowSize"), 1, offsetof(UiStyle, glowSize)},
    {DZ_OBF("gradient"), 1, offsetof(UiStyle, gradient)},
    {DZ_OBF("bevel"), 1, offsetof(UiStyle, bevel)},
    {DZ_OBF("hardShadow"), 2, offsetof(UiStyle, hardShadow)},
    {DZ_OBF("knobShadow"), 1, offsetof(UiStyle, knobShadow)},
};
static_assert(sizeof(kVarInfo) / sizeof(kVarInfo[0]) == size_t(UiStyleVar::Count), "kVarInfo must match UiStyleVar");

// Theme keys that are not style vars.
const VarInfo kExtraKeys[] = {
    {DZ_OBF("fontSize"), 1, offsetof(UiStyle, fontSize)},
    {DZ_OBF("disabledAlpha"), 1, offsetof(UiStyle, disabledAlpha)},
};

// Style values that are proportions, not sizes: ScaleAllSizes leaves them alone.
bool IsUnitless(size_t offset) {
    return offset == offsetof(UiStyle, alpha) || offset == offsetof(UiStyle, buttonTextAlign) ||
           offset == offsetof(UiStyle, gradient) || offset == offsetof(UiStyle, bevel) ||
           offset == offsetof(UiStyle, knobShadow);
}

// The shape settings: one-byte enums, written to theme files by name. Names are encrypted (detail::ObfEntry).
struct ShapeInfo {
    detail::ObfEntry name;
    size_t offset;
    const detail::ObfEntry* values;
    uint8_t count;
};
const detail::ObfEntry kFrameShapes[] = {DZ_OBF("Filled"), DZ_OBF("Outline"), DZ_OBF("Underline")};
const detail::ObfEntry kButtonShapes[] = {DZ_OBF("Filled"), DZ_OBF("Outline")};
const detail::ObfEntry kSliderShapes[] = {DZ_OBF("Block"), DZ_OBF("Rail"), DZ_OBF("Fill"), DZ_OBF("Segments")};
const detail::ObfEntry kCheckShapes[] = {DZ_OBF("Check"), DZ_OBF("Fill"), DZ_OBF("Square")};
const detail::ObfEntry kToggleShapes[] = {DZ_OBF("Pill"), DZ_OBF("Thin"), DZ_OBF("Square")};
const detail::ObfEntry kTabShapes[] = {DZ_OBF("Tab"), DZ_OBF("Underline"), DZ_OBF("Pill"), DZ_OBF("Box")};
const detail::ObfEntry kTitleShapes[] = {DZ_OBF("Bar"), DZ_OBF("Accent"), DZ_OBF("Plain"), DZ_OBF("Solid")};
const ShapeInfo kShapeInfo[] = {
    {DZ_OBF("frameShape"), offsetof(UiStyle, frameShape), kFrameShapes, 3},
    {DZ_OBF("buttonShape"), offsetof(UiStyle, buttonShape), kButtonShapes, 2},
    {DZ_OBF("sliderShape"), offsetof(UiStyle, sliderShape), kSliderShapes, 4},
    {DZ_OBF("checkShape"), offsetof(UiStyle, checkShape), kCheckShapes, 3},
    {DZ_OBF("toggleShape"), offsetof(UiStyle, toggleShape), kToggleShapes, 3},
    {DZ_OBF("tabShape"), offsetof(UiStyle, tabShape), kTabShapes, 4},
    {DZ_OBF("titleShape"), offsetof(UiStyle, titleShape), kTitleShapes, 4},
};

const detail::ObfEntry kColorNames[] = {
    DZ_OBF("Text"), DZ_OBF("TextDisabled"), DZ_OBF("WindowBg"), DZ_OBF("ChildBg"), DZ_OBF("PopupBg"), DZ_OBF("Border"),
    DZ_OBF("WindowShadow"), DZ_OBF("FrameBg"), DZ_OBF("FrameBgHovered"), DZ_OBF("FrameBgActive"), DZ_OBF("TitleBg"),
    DZ_OBF("TitleBgActive"), DZ_OBF("ScrollbarBg"), DZ_OBF("ScrollbarGrab"), DZ_OBF("ScrollbarGrabHovered"),
    DZ_OBF("ScrollbarGrabActive"), DZ_OBF("CheckMark"), DZ_OBF("SliderGrab"), DZ_OBF("SliderGrabActive"),
    DZ_OBF("Button"), DZ_OBF("ButtonHovered"), DZ_OBF("ButtonActive"), DZ_OBF("Header"), DZ_OBF("HeaderHovered"),
    DZ_OBF("HeaderActive"), DZ_OBF("Separator"), DZ_OBF("ResizeGrip"), DZ_OBF("ResizeGripHovered"),
    DZ_OBF("ResizeGripActive"), DZ_OBF("Tab"), DZ_OBF("TabHovered"), DZ_OBF("TabActive"), DZ_OBF("PlotLines"),
    DZ_OBF("PlotHistogram"), DZ_OBF("TextSelectedBg"), DZ_OBF("ModalDimBg"), DZ_OBF("MenuBarBg"),
    DZ_OBF("TableHeaderBg"), DZ_OBF("TableBorder"), DZ_OBF("TableRowBg"), DZ_OBF("TableRowBgAlt"), DZ_OBF("Glow"),
    DZ_OBF("AccentText"),
};
static_assert(sizeof(kColorNames) / sizeof(kColorNames[0]) == size_t(UiColor::Count), "kColorNames must match UiColor");

const detail::ObfEntry kThemeNames[] = {DZ_OBF("Dark"), DZ_OBF("Light"), DZ_OBF("Obsidian"), DZ_OBF("Cyberpunk"),
                                        DZ_OBF("Nord"), DZ_OBF("Emerald"), DZ_OBF("Crimson"), DZ_OBF("Dracula"),
                                        DZ_OBF("Sakura")};
static_assert(sizeof(kThemeNames) / sizeof(kThemeNames[0]) == size_t(UiTheme::Count), "kThemeNames must match UiTheme");
const detail::ObfEntry kLookNames[] = {DZ_OBF("Classic"), DZ_OBF("Soft"), DZ_OBF("Neon"),
                                       DZ_OBF("Flat"), DZ_OBF("Retro"), DZ_OBF("Glass")};
static_assert(sizeof(kLookNames) / sizeof(kLookNames[0]) == size_t(UiLook::Count), "kLookNames must match UiLook");

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? char(a[i] - 'A' + 'a') : a[i];
        const char y = (b[i] >= 'A' && b[i] <= 'Z') ? char(b[i] - 'A' + 'a') : b[i];
        if (x != y) return false;
    }
    return true;
}

// Index of `name` in `names` (case-insensitive), or -1. Decodes each encrypted entry into a scratch buffer, which is
// wiped before returning so the plaintext does not linger.
int FindName(std::string_view name, const detail::ObfEntry* names, int count) {
    char buf[detail::kObfCap];
    int found = -1;
    for (int i = 0; i < count && found < 0; ++i) {
        detail::ObfDecode(names[i], buf);
        if (EqualsNoCase(name, std::string_view(buf, names[i].len))) found = i;
    }
    detail::ObfWipe(buf, detail::kObfCap);
    return found;
}

// "#RRGGBB" or "#RRGGBBAA"
bool ParseHexColor(std::string_view s, Color& out) {
    if (s.size() != 7 && s.size() != 9) return false;
    uint32_t v = 0;
    for (size_t i = 1; i < s.size(); ++i) {
        const char c = s[i];
        const uint32_t d = (c >= '0' && c <= '9') ? uint32_t(c - '0')
                         : (c >= 'a' && c <= 'f') ? uint32_t(c - 'a' + 10)
                         : (c >= 'A' && c <= 'F') ? uint32_t(c - 'A' + 10)
                                                  : 16u;
        if (d > 15) return false;
        v = (v << 4) | d;
    }
    out = s.size() == 7 ? Hex(v) : Rgba(v >> 24, v >> 16, v >> 8, v);
    return true;
}

// Comma-separated floats; returns how many were read (up to `max`), or -1 on a malformed value.
int ParseFloats(std::string_view s, float* out, int max) {
    int count = 0;
    while (!s.empty()) {
        const size_t comma = s.find(',');
        const std::string token(Trim(s.substr(0, comma)));
        if (token.empty() || count == max) return -1;
        char* end = nullptr;
        out[count++] = std::strtof(token.c_str(), &end);
        if (end != token.c_str() + token.size()) return -1;
        if (comma == std::string_view::npos) break;
        s.remove_prefix(comma + 1);
    }
    return count;
}

void SetDarkColors(Color* c) {
    const Color accent = Hex(0x5B8CFF);
    c[size_t(UiColor::Text)] = Hex(0xE6E8EF);
    c[size_t(UiColor::TextDisabled)] = Hex(0x7C8090);
    c[size_t(UiColor::WindowBg)] = Hex(0x1B1D24, 0xF2);
    c[size_t(UiColor::ChildBg)] = Rgba(0, 0, 0, 0);
    c[size_t(UiColor::PopupBg)] = Hex(0x20232B, 0xFA);
    c[size_t(UiColor::Border)] = Rgba(255, 255, 255, 26);
    c[size_t(UiColor::WindowShadow)] = Rgba(0, 0, 0, 150);
    c[size_t(UiColor::FrameBg)] = Hex(0x2A2E39);
    c[size_t(UiColor::FrameBgHovered)] = Hex(0x343947);
    c[size_t(UiColor::FrameBgActive)] = Hex(0x3C4252);
    c[size_t(UiColor::TitleBg)] = Hex(0x15171D);
    c[size_t(UiColor::TitleBgActive)] = Hex(0x1F2433);
    c[size_t(UiColor::ScrollbarBg)] = Rgba(0, 0, 0, 0);
    c[size_t(UiColor::ScrollbarGrab)] = Hex(0x3A3F4D);
    c[size_t(UiColor::ScrollbarGrabHovered)] = Hex(0x4A5062);
    c[size_t(UiColor::ScrollbarGrabActive)] = Hex(0x5A6175);
    c[size_t(UiColor::CheckMark)] = accent;
    c[size_t(UiColor::SliderGrab)] = accent;
    c[size_t(UiColor::SliderGrabActive)] = Hex(0x86A9FF);
    c[size_t(UiColor::Button)] = Hex(0x2F3442);
    c[size_t(UiColor::ButtonHovered)] = Hex(0x3D4457);
    c[size_t(UiColor::ButtonActive)] = Hex(0x4B6BD6);
    c[size_t(UiColor::Header)] = WithAlpha(accent, 0x45);
    c[size_t(UiColor::HeaderHovered)] = WithAlpha(accent, 0x70);
    c[size_t(UiColor::HeaderActive)] = WithAlpha(accent, 0xA0);
    c[size_t(UiColor::Separator)] = Rgba(255, 255, 255, 30);
    c[size_t(UiColor::ResizeGrip)] = WithAlpha(accent, 0x30);
    c[size_t(UiColor::ResizeGripHovered)] = WithAlpha(accent, 0xA0);
    c[size_t(UiColor::ResizeGripActive)] = WithAlpha(accent, 0xE0);
    c[size_t(UiColor::Tab)] = Hex(0x262A35);
    c[size_t(UiColor::TabHovered)] = Hex(0x3D4457);
    c[size_t(UiColor::TabActive)] = Hex(0x3B57B8);
    c[size_t(UiColor::PlotLines)] = accent;
    c[size_t(UiColor::PlotHistogram)] = accent;
    c[size_t(UiColor::TextSelectedBg)] = WithAlpha(accent, 0x66);
    c[size_t(UiColor::ModalDimBg)] = Rgba(0, 0, 0, 140);
    c[size_t(UiColor::MenuBarBg)] = Hex(0x20232D);
    c[size_t(UiColor::TableHeaderBg)] = Hex(0x262A35);
    c[size_t(UiColor::TableBorder)] = Rgba(255, 255, 255, 30);
    c[size_t(UiColor::TableRowBg)] = Rgba(0, 0, 0, 0);
    c[size_t(UiColor::TableRowBgAlt)] = Rgba(255, 255, 255, 9);
    c[size_t(UiColor::Glow)] = WithAlpha(accent, 0xB0);
    c[size_t(UiColor::AccentText)] = colors::White;
}

void SetLightColors(Color* c) {
    const Color accent = Hex(0x3567E8);
    c[size_t(UiColor::Text)] = Hex(0x1C1E24);
    c[size_t(UiColor::TextDisabled)] = Hex(0x8A8E99);
    c[size_t(UiColor::WindowBg)] = Hex(0xF4F5F8, 0xF8);
    c[size_t(UiColor::ChildBg)] = Rgba(0, 0, 0, 0);
    c[size_t(UiColor::PopupBg)] = Hex(0xFFFFFF, 0xFC);
    c[size_t(UiColor::Border)] = Rgba(0, 0, 0, 30);
    c[size_t(UiColor::WindowShadow)] = Rgba(0, 0, 0, 70);
    c[size_t(UiColor::FrameBg)] = Hex(0xE3E5EB);
    c[size_t(UiColor::FrameBgHovered)] = Hex(0xD7DAE2);
    c[size_t(UiColor::FrameBgActive)] = Hex(0xCBCFD9);
    c[size_t(UiColor::TitleBg)] = Hex(0xE6E8EE);
    c[size_t(UiColor::TitleBgActive)] = Hex(0xD3DBF2);
    c[size_t(UiColor::ScrollbarBg)] = Rgba(0, 0, 0, 0);
    c[size_t(UiColor::ScrollbarGrab)] = Hex(0xC4C8D2);
    c[size_t(UiColor::ScrollbarGrabHovered)] = Hex(0xADB2BF);
    c[size_t(UiColor::ScrollbarGrabActive)] = Hex(0x969CAB);
    c[size_t(UiColor::CheckMark)] = accent;
    c[size_t(UiColor::SliderGrab)] = accent;
    c[size_t(UiColor::SliderGrabActive)] = Hex(0x2550C4);
    c[size_t(UiColor::Button)] = Hex(0xDCDFE7);
    c[size_t(UiColor::ButtonHovered)] = Hex(0xCDD2DD);
    c[size_t(UiColor::ButtonActive)] = WithAlpha(accent, 0x90);
    c[size_t(UiColor::Header)] = WithAlpha(accent, 0x30);
    c[size_t(UiColor::HeaderHovered)] = WithAlpha(accent, 0x50);
    c[size_t(UiColor::HeaderActive)] = WithAlpha(accent, 0x78);
    c[size_t(UiColor::Separator)] = Rgba(0, 0, 0, 35);
    c[size_t(UiColor::ResizeGrip)] = WithAlpha(accent, 0x30);
    c[size_t(UiColor::ResizeGripHovered)] = WithAlpha(accent, 0x90);
    c[size_t(UiColor::ResizeGripActive)] = WithAlpha(accent, 0xD0);
    c[size_t(UiColor::Tab)] = Hex(0xE3E5EB);
    c[size_t(UiColor::TabHovered)] = Hex(0xCDD2DD);
    c[size_t(UiColor::TabActive)] = Hex(0xB9C9F5);
    c[size_t(UiColor::PlotLines)] = accent;
    c[size_t(UiColor::PlotHistogram)] = accent;
    c[size_t(UiColor::TextSelectedBg)] = WithAlpha(accent, 0x55);
    c[size_t(UiColor::ModalDimBg)] = Rgba(30, 30, 40, 90);
    c[size_t(UiColor::MenuBarBg)] = Hex(0xE6E8EE);
    c[size_t(UiColor::TableHeaderBg)] = Hex(0xE3E5EB);
    c[size_t(UiColor::TableBorder)] = Rgba(0, 0, 0, 30);
    c[size_t(UiColor::TableRowBg)] = Rgba(0, 0, 0, 0);
    c[size_t(UiColor::TableRowBgAlt)] = Rgba(0, 0, 0, 10);
    c[size_t(UiColor::Glow)] = WithAlpha(accent, 0x80);
    c[size_t(UiColor::AccentText)] = colors::White;
}

// The sizes, shapes and effects of each look, applied over the classic defaults (the UiStyle member initializers).
void SetLook(UiStyle& s, UiLook look) {
    switch (look) {
    case UiLook::Soft:
        s.windowPadding = {14.0f, 12.0f};
        s.windowRounding = 14.0f;
        s.windowShadowSize = 32.0f;
        s.framePadding = {10.0f, 6.0f};
        s.frameRounding = 8.0f;
        s.itemSpacing = {10.0f, 8.0f};
        s.itemInnerSpacing = {8.0f, 6.0f};
        s.scrollbarSize = 10.0f;
        s.scrollbarRounding = 5.0f;
        s.grabMinSize = 14.0f;
        s.grabRounding = 8.0f;
        s.popupRounding = 10.0f;
        s.tabRounding = 8.0f;
        s.sliderShape = SliderShape::Rail;
        s.checkShape = CheckShape::Fill;
        s.tabShape = TabShape::Pill;
        s.titleShape = TitleShape::Plain;
        s.knobShadow = 0.8f;
        break;
    case UiLook::Neon:
        s.windowPadding = {12.0f, 10.0f};
        s.windowRounding = 4.0f;
        s.windowShadowSize = 22.0f;
        s.frameRounding = 3.0f;
        s.frameBorderSize = 1.0f;
        s.itemSpacing = {8.0f, 7.0f};
        s.scrollbarSize = 8.0f;
        s.scrollbarRounding = 4.0f;
        s.grabRounding = 2.0f;
        s.popupRounding = 4.0f;
        s.tabRounding = 3.0f;
        s.frameShape = FrameShape::Outline;
        s.buttonShape = ButtonShape::Outline;
        s.sliderShape = SliderShape::Rail;
        s.checkShape = CheckShape::Fill;
        s.tabShape = TabShape::Underline;
        s.titleShape = TitleShape::Accent;
        s.glowSize = 10.0f;
        break;
    case UiLook::Flat:
        s.windowPadding = {16.0f, 14.0f};
        s.windowRounding = 2.0f;
        s.windowBorderSize = 0.0f;
        s.windowShadowSize = 16.0f;
        s.framePadding = {8.0f, 6.0f};
        s.frameRounding = 2.0f;
        s.itemSpacing = {12.0f, 10.0f};
        s.scrollbarSize = 6.0f;
        s.scrollbarRounding = 3.0f;
        s.grabMinSize = 10.0f;
        s.grabRounding = 2.0f;
        s.popupRounding = 2.0f;
        s.tabRounding = 0.0f;
        s.frameShape = FrameShape::Underline;
        s.sliderShape = SliderShape::Rail;
        s.checkShape = CheckShape::Fill;
        s.toggleShape = ToggleShape::Thin;
        s.tabShape = TabShape::Underline;
        s.titleShape = TitleShape::Plain;
        s.knobShadow = 0.6f;
        break;
    case UiLook::Retro:
        s.windowRounding = 0.0f;
        s.windowBorderSize = 2.0f;
        s.windowShadowSize = 0.0f;
        s.framePadding = {8.0f, 4.0f};
        s.frameRounding = 0.0f;
        s.frameBorderSize = 1.0f;
        s.scrollbarSize = 14.0f;
        s.scrollbarRounding = 0.0f;
        s.grabRounding = 0.0f;
        s.popupRounding = 0.0f;
        s.tabRounding = 0.0f;
        s.frameShape = FrameShape::Outline;
        s.sliderShape = SliderShape::Segments;
        s.checkShape = CheckShape::Square;
        s.toggleShape = ToggleShape::Square;
        s.tabShape = TabShape::Box;
        s.titleShape = TitleShape::Solid;
        s.hardShadow = {5.0f, 5.0f};
        break;
    case UiLook::Glass:
        s.windowPadding = {14.0f, 12.0f};
        s.windowRounding = 12.0f;
        s.windowShadowSize = 28.0f;
        s.framePadding = {10.0f, 6.0f};
        s.frameRounding = 7.0f;
        s.frameBorderSize = 1.0f;
        s.itemSpacing = {10.0f, 8.0f};
        s.scrollbarSize = 10.0f;
        s.scrollbarRounding = 5.0f;
        s.grabRounding = 7.0f;
        s.popupRounding = 10.0f;
        s.tabRounding = 7.0f;
        s.sliderShape = SliderShape::Fill;
        s.checkShape = CheckShape::Fill;
        s.tabShape = TabShape::Pill;
        s.gradient = 0.35f;
        s.bevel = 0.45f;
        s.knobShadow = 0.5f;
        break;
    default:
        break;
    }
}

} // namespace

namespace ui_detail {
const VarInfo& GetVarInfo(UiStyleVar var) { return kVarInfo[size_t(var)]; }

Color LerpColor(Color a, Color b, float t) {
    auto channel = [&](int shift) {
        const float x = float((a >> shift) & 0xFFu), y = float((b >> shift) & 0xFFu);
        return uint32_t(x + (y - x) * t + 0.5f) << shift;
    };
    return channel(0) | channel(8) | channel(16) | channel(24);
}
} // namespace ui_detail

UiStyle::UiStyle() { SetDarkColors(colors); }

UiStyle UiStyle::Dark() { return UiStyle(); }

UiStyle UiStyle::Light() {
    UiStyle style;
    SetLightColors(style.colors);
    style.windowShadowSize = 14.0f;
    return style;
}

UiStyle UiStyle::Make(UiTheme theme, UiLook look) {
    UiStyle style;
    style.ApplyLook(look);
    style.ApplyTheme(theme);
    return style;
}

const char* UiStyle::ThemeName(UiTheme theme) {
    return size_t(theme) < size_t(UiTheme::Count) ? detail::ObfGet(kThemeNames[size_t(theme)]) : "";
}

const char* UiStyle::LookName(UiLook look) {
    return size_t(look) < size_t(UiLook::Count) ? detail::ObfGet(kLookNames[size_t(look)]) : "";
}

UiPalette UiStyle::ThemePalette(UiTheme theme) {
    UiPalette p;
    switch (theme) {
    case UiTheme::Light:
        p = {Hex(0xF4F5F8, 0xF8), Hex(0xE3E5EB), Hex(0x1C1E24), Hex(0x3567E8), colors::White, Hex(0x3567E8, 0x80),
             Rgba(0, 0, 0, 30), Rgba(0, 0, 0, 70), 0, true};
        break;
    case UiTheme::Obsidian:
        p = {Hex(0x0B0B0C, 0xF5), Hex(0x1E1E21), Hex(0xEDEDED), Hex(0xFFD60A), Hex(0x111111), Hex(0xFFD60A, 0xB0),
             Rgba(255, 255, 255, 24), Rgba(0, 0, 0, 210), 0, false};
        break;
    case UiTheme::Cyberpunk:
        p = {Hex(0x0E0A1F, 0xF2), Hex(0x1E1638), Hex(0xF3EAFF), Hex(0xFF2A6D), Hex(0x14061E), Hex(0xFF2A6D, 0xC0),
             Hex(0xFF2A6D, 0x50), Rgba(0, 0, 0, 190), Hex(0x05D9E8), false};
        break;
    case UiTheme::Nord:
        p = {Hex(0x2E3440, 0xF6), Hex(0x3B4252), Hex(0xECEFF4), Hex(0x88C0D0), Hex(0x2E3440), Hex(0x88C0D0, 0xA0),
             Hex(0x4C566A), Rgba(0, 0, 0, 140), Hex(0xA3BE8C), false};
        break;
    case UiTheme::Emerald:
        p = {Hex(0x07110C, 0xF4), Hex(0x10241A), Hex(0xD4F5E0), Hex(0x2BE38B), Hex(0x04140B), Hex(0x2BE38B, 0xB0),
             Hex(0x2BE38B, 0x38), Rgba(0, 0, 0, 190), 0, false};
        break;
    case UiTheme::Crimson:
        p = {Hex(0x151113, 0xF4), Hex(0x271E22), Hex(0xF3E8EA), Hex(0xE5383B), colors::White, Hex(0xFF4D5A, 0xA8),
             Rgba(255, 255, 255, 22), Rgba(0, 0, 0, 180), Hex(0xF4A259), false};
        break;
    case UiTheme::Dracula:
        p = {Hex(0x282A36, 0xF6), Hex(0x3A3C4E), Hex(0xF8F8F2), Hex(0xBD93F9), Hex(0x282A36), Hex(0xFF79C6, 0xA8),
             Hex(0x6272A4, 0x80), Rgba(0, 0, 0, 150), Hex(0xFF79C6), false};
        break;
    case UiTheme::Sakura:
        p = {Hex(0xFFF4F7, 0xF8), Hex(0xF9DDE6), Hex(0x3B2530), Hex(0xE0457B), colors::White, Hex(0xFF6F9F, 0x90),
             Rgba(120, 30, 60, 40), Rgba(90, 20, 50, 70), 0, true};
        break;
    default:  // Dark
        p = {Hex(0x1B1D24, 0xF2), Hex(0x2A2E39), Hex(0xE6E8EF), Hex(0x5B8CFF), colors::White, Hex(0x5B8CFF, 0xB0),
             Rgba(255, 255, 255, 26), Rgba(0, 0, 0, 150), 0, false};
        break;
    }
    return p;
}

void UiStyle::ApplyTheme(UiTheme theme) {
    // Dark and Light keep their hand-tuned colors; the others are derived from their palettes.
    if (theme == UiTheme::Dark) SetDarkColors(colors);
    else if (theme == UiTheme::Light) SetLightColors(colors);
    else ApplyPalette(ThemePalette(theme));
}

void UiStyle::ApplyPalette(const UiPalette& p) {
    Color* c = colors;
    const Color bg = WithAlpha(p.background, 255);
    const Color surface = p.surface, text = p.text, accent = p.accent;
    auto set = [&](UiColor color, Color value) { c[size_t(color)] = value; };
    auto mix = [](Color a, Color b, float t) { return LerpColor(a, b, t); };
    set(UiColor::Text, text);
    set(UiColor::TextDisabled, mix(text, bg, 0.48f));
    set(UiColor::WindowBg, p.background);
    set(UiColor::ChildBg, 0);
    set(UiColor::PopupBg, WithAlpha(mix(bg, surface, 0.25f), std::max(ColorAlpha(p.background), 0xF8u)));
    set(UiColor::Border, p.border);
    set(UiColor::WindowShadow, p.shadow);
    set(UiColor::FrameBg, surface);
    set(UiColor::FrameBgHovered, mix(surface, text, 0.08f));
    set(UiColor::FrameBgActive, mix(surface, text, 0.14f));
    const Color title = p.light ? mix(bg, text, 0.05f) : mix(bg, colors::Black, 0.25f);
    set(UiColor::TitleBg, title);
    set(UiColor::TitleBgActive, mix(title, accent, p.light ? 0.16f : 0.12f));
    set(UiColor::ScrollbarBg, 0);
    set(UiColor::ScrollbarGrab, mix(surface, text, 0.14f));
    set(UiColor::ScrollbarGrabHovered, mix(surface, text, 0.24f));
    set(UiColor::ScrollbarGrabActive, mix(surface, text, 0.34f));
    set(UiColor::CheckMark, accent);
    set(UiColor::SliderGrab, accent);
    set(UiColor::SliderGrabActive, mix(accent, p.light ? colors::Black : colors::White, 0.25f));
    set(UiColor::Button, mix(surface, text, 0.04f));
    set(UiColor::ButtonHovered, mix(surface, text, 0.12f));
    set(UiColor::ButtonActive, mix(surface, accent, 0.5f));
    set(UiColor::Header, WithAlpha(accent, 0x40));
    set(UiColor::HeaderHovered, WithAlpha(accent, 0x68));
    set(UiColor::HeaderActive, WithAlpha(accent, 0x98));
    set(UiColor::Separator, WithAlpha(text, 0x24));
    set(UiColor::ResizeGrip, WithAlpha(accent, 0x30));
    set(UiColor::ResizeGripHovered, WithAlpha(accent, 0xA0));
    set(UiColor::ResizeGripActive, WithAlpha(accent, 0xE0));
    set(UiColor::Tab, mix(surface, bg, 0.35f));
    set(UiColor::TabHovered, mix(surface, text, 0.12f));
    set(UiColor::TabActive, mix(surface, accent, 0.45f));
    set(UiColor::PlotLines, p.secondary ? p.secondary : accent);
    set(UiColor::PlotHistogram, accent);
    set(UiColor::TextSelectedBg, WithAlpha(accent, 0x60));
    set(UiColor::ModalDimBg, p.light ? Rgba(30, 30, 40, 90) : Rgba(0, 0, 0, 140));
    set(UiColor::MenuBarBg, mix(bg, surface, 0.5f));
    set(UiColor::TableHeaderBg, mix(surface, bg, 0.2f));
    set(UiColor::TableBorder, WithAlpha(text, 0x22));
    set(UiColor::TableRowBg, 0);
    set(UiColor::TableRowBgAlt, WithAlpha(text, 0x0A));
    set(UiColor::Glow, p.glow);
    set(UiColor::AccentText, p.accentText);
}

void UiStyle::ApplyLook(UiLook look) {
    UiStyle s;  // the classic sizes, shapes and effects
    SetLook(s, look);
    s.font = font;
    s.fontSize = fontSize;
    s.alpha = alpha;
    s.disabledAlpha = disabledAlpha;
    std::memcpy(s.colors, colors, sizeof(colors));
    *this = s;
}

void UiStyle::ScaleAllSizes(float scale) {
    for (const VarInfo& info : kVarInfo) {
        if (IsUnitless(info.offset)) continue;
        float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(this) + info.offset);
        for (int i = 0; i < info.count; ++i) field[i] = std::floor(field[i] * scale);
    }
}

const char* UiStyle::ColorName(UiColor color) { return detail::ObfGet(kColorNames[size_t(color)]); }
const char* UiStyle::VarName(UiStyleVar var) { return detail::ObfGet(kVarInfo[size_t(var)].name); }

bool UiStyle::LoadTheme(std::string_view text, std::string* error) {
    bool ok = true;
    int lineNumber = 0;
    auto report = [&](const char* what, std::string_view line) {
        ok = false;
        if (!error) return;
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), DZ_STR("line %d: %s: "), lineNumber, what);
        *error += prefix;
        *error += line;
        *error += '\n';
    };
    while (!text.empty()) {
        const size_t newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view() : text.substr(newline + 1);
        ++lineNumber;
        // Comments are whole lines: a '#' after '=' starts a hex color.
        if (Trim(line).empty() || Trim(line).front() == '#') continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            report(DZ_STR("expected 'name = value'"), line);
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));

        if (key.substr(0, 6) == DZ_STR("color.")) {
            const std::string_view name = key.substr(6);
            size_t index = size_t(UiColor::Count);
            for (size_t i = 0; i < size_t(UiColor::Count); ++i) {
                if (detail::ObfEq(kColorNames[i], name)) index = i;
            }
            Color color;
            if (index == size_t(UiColor::Count)) report(DZ_STR("unknown color"), line);
            else if (!ParseHexColor(value, color)) report(DZ_STR("expected #RRGGBB or #RRGGBBAA"), line);
            else colors[index] = color;
            continue;
        }
        if (key == DZ_STR("theme") || key == DZ_STR("look")) {
            const bool isTheme = key == DZ_STR("theme");
            const int index = isTheme ? FindName(value, kThemeNames, int(UiTheme::Count))
                                      : FindName(value, kLookNames, int(UiLook::Count));
            if (index < 0) {
                if (isTheme) report(DZ_STR("unknown theme"), line);
                else report(DZ_STR("unknown look"), line);
            } else if (isTheme) ApplyTheme(UiTheme(index));
            else ApplyLook(UiLook(index));
            continue;
        }
        const ShapeInfo* shape = nullptr;
        for (const ShapeInfo& s : kShapeInfo) {
            if (detail::ObfEq(s.name, key)) shape = &s;
        }
        if (shape) {
            const int index = FindName(value, shape->values, shape->count);
            if (index < 0) report(DZ_STR("unknown shape"), line);
            else *(reinterpret_cast<uint8_t*>(this) + shape->offset) = uint8_t(index);
            continue;
        }
        const VarInfo* info = nullptr;
        for (const VarInfo& v : kVarInfo) {
            if (detail::ObfEq(v.name, key)) info = &v;
        }
        for (const VarInfo& v : kExtraKeys) {
            if (detail::ObfEq(v.name, key)) info = &v;
        }
        if (!info) {
            report(DZ_STR("unknown setting"), line);
            continue;
        }
        float values[2];
        if (ParseFloats(value, values, 2) != info->count) {
            if (info->count == 2) report(DZ_STR("expected two numbers 'x, y'"), line);
            else report(DZ_STR("expected a number"), line);
            continue;
        }
        float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(this) + info->offset);
        for (int i = 0; i < info->count; ++i) field[i] = values[i];
    }
    return ok;
}

std::string UiStyle::SaveTheme() const {
    std::string out(DZ_STR("# drizzy_renderer UI theme\n"));
    char line[128];
    auto writeVar = [&](const VarInfo& info) {
        const float* field = reinterpret_cast<const float*>(reinterpret_cast<const char*>(this) + info.offset);
        if (info.count == 2) std::snprintf(line, sizeof(line), DZ_STR("%s = %g, %g\n"), detail::ObfGet(info.name), double(field[0]), double(field[1]));
        else std::snprintf(line, sizeof(line), DZ_STR("%s = %g\n"), detail::ObfGet(info.name), double(field[0]));
        out += line;
    };
    for (const VarInfo& info : kExtraKeys) writeVar(info);
    for (const VarInfo& info : kVarInfo) writeVar(info);
    for (const ShapeInfo& shape : kShapeInfo) {
        const uint8_t value = *(reinterpret_cast<const uint8_t*>(this) + shape.offset);
        const char* shapeName = detail::ObfGet(shape.name);
        std::snprintf(line, sizeof(line), DZ_STR("%s = %s\n"), shapeName, detail::ObfGet(shape.values[value < shape.count ? value : 0]));
        out += line;
    }
    for (size_t i = 0; i < size_t(UiColor::Count); ++i) {
        const Color c = colors[i];
        std::snprintf(line, sizeof(line), DZ_STR("color.%s = #%02X%02X%02X%02X\n"), detail::ObfGet(kColorNames[i]),
                      c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24);
        out += line;
    }
    return out;
}

// =====================================================================================================================
// UiInput
// =====================================================================================================================
void UiInput::AddCharacter(uint32_t codepoint) {
    if (codepoint != 0 && codepoint <= 0x10FFFF) characters.push_back(codepoint);
}

void UiInput::AddCharactersUtf8(std::string_view text) {
    const char* end = text.data() + text.size();
    for (const char* p = text.data(); p < end;) AddCharacter(detail::DecodeUtf8(p, end));
}

} // namespace drizzy
