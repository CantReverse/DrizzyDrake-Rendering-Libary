// UI styles: built-in themes, DPI scaling, and the text theme format.
#include "ui_internal.h"

#include "../text_layout.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace drizzy {
using namespace ui_detail;

namespace {

// Indexed by UiStyleVar; the names double as theme file keys.
const VarInfo kVarInfo[] = {
    {"alpha", 1, offsetof(UiStyle, alpha)},
    {"windowPadding", 2, offsetof(UiStyle, windowPadding)},
    {"windowRounding", 1, offsetof(UiStyle, windowRounding)},
    {"windowBorderSize", 1, offsetof(UiStyle, windowBorderSize)},
    {"windowShadowSize", 1, offsetof(UiStyle, windowShadowSize)},
    {"windowMinSize", 2, offsetof(UiStyle, windowMinSize)},
    {"framePadding", 2, offsetof(UiStyle, framePadding)},
    {"frameRounding", 1, offsetof(UiStyle, frameRounding)},
    {"frameBorderSize", 1, offsetof(UiStyle, frameBorderSize)},
    {"itemSpacing", 2, offsetof(UiStyle, itemSpacing)},
    {"itemInnerSpacing", 2, offsetof(UiStyle, itemInnerSpacing)},
    {"indentSpacing", 1, offsetof(UiStyle, indentSpacing)},
    {"scrollbarSize", 1, offsetof(UiStyle, scrollbarSize)},
    {"scrollbarRounding", 1, offsetof(UiStyle, scrollbarRounding)},
    {"grabMinSize", 1, offsetof(UiStyle, grabMinSize)},
    {"grabRounding", 1, offsetof(UiStyle, grabRounding)},
    {"popupRounding", 1, offsetof(UiStyle, popupRounding)},
    {"tabRounding", 1, offsetof(UiStyle, tabRounding)},
    {"buttonTextAlign", 2, offsetof(UiStyle, buttonTextAlign)},
    {"cellPadding", 2, offsetof(UiStyle, cellPadding)},
};
static_assert(sizeof(kVarInfo) / sizeof(kVarInfo[0]) == size_t(UiStyleVar::Count), "kVarInfo must match UiStyleVar");

// Theme keys that are not style vars.
const VarInfo kExtraKeys[] = {
    {"fontSize", 1, offsetof(UiStyle, fontSize)},
    {"disabledAlpha", 1, offsetof(UiStyle, disabledAlpha)},
};

const char* const kColorNames[] = {
    "Text", "TextDisabled", "WindowBg", "ChildBg", "PopupBg", "Border", "WindowShadow", "FrameBg",
    "FrameBgHovered", "FrameBgActive", "TitleBg", "TitleBgActive", "ScrollbarBg", "ScrollbarGrab",
    "ScrollbarGrabHovered", "ScrollbarGrabActive", "CheckMark", "SliderGrab", "SliderGrabActive", "Button",
    "ButtonHovered", "ButtonActive", "Header", "HeaderHovered", "HeaderActive", "Separator", "ResizeGrip",
    "ResizeGripHovered", "ResizeGripActive", "Tab", "TabHovered", "TabActive", "PlotLines", "PlotHistogram",
    "TextSelectedBg", "ModalDimBg", "MenuBarBg", "TableHeaderBg", "TableBorder", "TableRowBg", "TableRowBgAlt",
};
static_assert(sizeof(kColorNames) / sizeof(kColorNames[0]) == size_t(UiColor::Count), "kColorNames must match UiColor");

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
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
}

} // namespace

namespace ui_detail {
const VarInfo& GetVarInfo(UiStyleVar var) { return kVarInfo[size_t(var)]; }
} // namespace ui_detail

UiStyle::UiStyle() { SetDarkColors(colors); }

UiStyle UiStyle::Dark() { return UiStyle(); }

UiStyle UiStyle::Light() {
    UiStyle style;
    SetLightColors(style.colors);
    style.windowShadowSize = 14.0f;
    return style;
}

void UiStyle::ScaleAllSizes(float scale) {
    for (const VarInfo& info : kVarInfo) {
        if (info.offset == offsetof(UiStyle, alpha) || info.offset == offsetof(UiStyle, buttonTextAlign)) continue;
        float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(this) + info.offset);
        for (int i = 0; i < info.count; ++i) field[i] = std::floor(field[i] * scale);
    }
}

const char* UiStyle::ColorName(UiColor color) { return kColorNames[size_t(color)]; }
const char* UiStyle::VarName(UiStyleVar var) { return kVarInfo[size_t(var)].name; }

bool UiStyle::LoadTheme(std::string_view text, std::string* error) {
    bool ok = true;
    int lineNumber = 0;
    auto report = [&](const char* what, std::string_view line) {
        ok = false;
        if (!error) return;
        char prefix[64];
        std::snprintf(prefix, sizeof(prefix), "line %d: %s: ", lineNumber, what);
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
            report("expected 'name = value'", line);
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));

        if (key.substr(0, 6) == "color.") {
            const std::string_view name = key.substr(6);
            size_t index = size_t(UiColor::Count);
            for (size_t i = 0; i < size_t(UiColor::Count); ++i) {
                if (name == kColorNames[i]) index = i;
            }
            Color color;
            if (index == size_t(UiColor::Count)) report("unknown color", line);
            else if (!ParseHexColor(value, color)) report("expected #RRGGBB or #RRGGBBAA", line);
            else colors[index] = color;
            continue;
        }
        const VarInfo* info = nullptr;
        for (const VarInfo& v : kVarInfo) {
            if (key == v.name) info = &v;
        }
        for (const VarInfo& v : kExtraKeys) {
            if (key == v.name) info = &v;
        }
        if (!info) {
            report("unknown setting", line);
            continue;
        }
        float values[2];
        if (ParseFloats(value, values, 2) != info->count) {
            report(info->count == 2 ? "expected two numbers 'x, y'" : "expected a number", line);
            continue;
        }
        float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(this) + info->offset);
        for (int i = 0; i < info->count; ++i) field[i] = values[i];
    }
    return ok;
}

std::string UiStyle::SaveTheme() const {
    std::string out = "# drizzy_renderer UI theme\n";
    char line[128];
    auto writeVar = [&](const VarInfo& info) {
        const float* field = reinterpret_cast<const float*>(reinterpret_cast<const char*>(this) + info.offset);
        if (info.count == 2) std::snprintf(line, sizeof(line), "%s = %g, %g\n", info.name, double(field[0]), double(field[1]));
        else std::snprintf(line, sizeof(line), "%s = %g\n", info.name, double(field[0]));
        out += line;
    };
    for (const VarInfo& info : kExtraKeys) writeVar(info);
    for (const VarInfo& info : kVarInfo) writeVar(info);
    for (size_t i = 0; i < size_t(UiColor::Count); ++i) {
        const Color c = colors[i];
        std::snprintf(line, sizeof(line), "color.%s = #%02X%02X%02X%02X\n", kColorNames[i], c & 0xFFu, (c >> 8) & 0xFFu,
                      (c >> 16) & 0xFFu, c >> 24);
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
