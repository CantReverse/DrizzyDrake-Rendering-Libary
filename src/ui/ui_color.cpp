// Color widgets: swatch button, RGB(A) edit, and a saturation/value + hue (+ alpha) picker.
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace drizzy {
using namespace ui_detail;

namespace {

void RgbToHsv(float r, float g, float b, float& h, float& s, float& v) {
    const float mx = std::max({r, g, b}), mn = std::min({r, g, b});
    v = mx;
    const float d = mx - mn;
    s = mx <= 0.0f ? 0.0f : d / mx;
    if (d <= 0.0f) {
        h = 0.0f;
        return;
    }
    if (mx == r) h = (g - b) / d + (g < b ? 6.0f : 0.0f);
    else if (mx == g) h = (b - r) / d + 2.0f;
    else h = (r - g) / d + 4.0f;
    h /= 6.0f;
}

void HsvToRgb(float h, float s, float v, float& r, float& g, float& b) {
    if (s <= 0.0f) {
        r = g = b = v;
        return;
    }
    h = (h - std::floor(h)) * 6.0f;
    const int i = int(h);
    const float f = h - float(i);
    const float p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
}

Color ColorFromHsv(float h, float s, float v, uint32_t alpha) {
    float r, g, b;
    HsvToRgb(h, s, v, r, g, b);
    return WithAlpha(RgbaF(r, g, b), alpha);
}

// Draws a light/dark checkerboard so a translucent swatch reads as translucent. The cells are cut to `r` themselves,
// so no clip rect (and no extra draw command) is needed; cells that would poke out of the rounded corners are left out.
void DrawChecker(DrawList& dl, const Rect& r, const CornerRadii& radii) {
    dl.AddRectFilled(r, Rgba(130, 130, 130), radii);
    const float cell = std::max(4.0f, std::floor(r.Height() * 0.25f));
    bool row = false;
    for (float y = r.min.y; y < r.max.y; y += cell, row = !row) {
        for (float x = r.min.x + (row ? cell : 0.0f); x < r.max.x; x += cell * 2.0f) {
            const Rect c(x, y, std::min(x + cell, r.max.x), std::min(y + cell, r.max.y));
            const bool top = c.min.y < r.min.y + 1.0f, bottom = c.max.y > r.max.y - 1.0f;
            const bool left = c.min.x < r.min.x + 1.0f, right = c.max.x > r.max.x - 1.0f;
            if ((top && left && radii.tl > 1.0f) || (top && right && radii.tr > 1.0f) ||
                (bottom && right && radii.br > 1.0f) || (bottom && left && radii.bl > 1.0f)) {
                continue;
            }
            dl.AddRectFilled(c, Rgba(90, 90, 90));
        }
    }
}

// A swatch: the opaque color, or (translucent colors) the opaque color on the left and the translucent one over a
// checkerboard on the right.
void DrawSwatch(UiState& g, const Rect& bb, Color color, float rounding, bool showAlpha) {
    DrawList& dl = *g.current->dl;
    const Color opaque = WithAlpha(color, uint32_t(255.0f * g.alpha + 0.5f));
    if (!showAlpha || ColorAlpha(color) == 255) {
        dl.AddRectFilled(bb, opaque, rounding);
        return;
    }
    const float mid = std::floor(bb.Center().x);
    dl.AddRectFilled(Rect(bb.min.x, bb.min.y, mid, bb.max.y), opaque, CornerRadii(rounding, 0.0f, 0.0f, rounding));
    const Rect right(mid, bb.min.y, bb.max.x, bb.max.y);
    const CornerRadii radii(0.0f, rounding, rounding, 0.0f);
    DrawChecker(dl, right, radii);
    dl.AddRectFilled(right, ScaleAlpha(color, g.alpha), radii);
}

} // namespace

bool Ui::ColorButton(std::string_view strId, Color color, Vec2 sizeArg, uint32_t flags) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const ID id = HashStr(strId, w->idStack.back());
    const float h = FrameHeight(g);
    const Vec2 size = CalcItemSize(g, sizeArg, h, h);
    const Rect bb = Rect::FromPosSize(w->cursorPos, size);
    ItemSize(g, size, g.style.framePadding.y);
    if (!ItemAdd(g, bb, id)) return false;
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, bb, id, &hovered, &held);
    const float rounding = std::min(g.style.frameRounding, size.y * 0.5f);
    if (hovered) RenderGlow(g, bb, rounding, 0.6f);
    DrawSwatch(g, bb, color, rounding, !(flags & ColorEditFlags::NoAlpha));
    w->dl->AddRect(bb, hovered ? StyleColor(g, UiColor::Text, 0.8f) : StyleColor(g, UiColor::Border), rounding, 1.0f);
    return pressed;
}

bool Ui::ColorPicker4(std::string_view label, Color* color, uint32_t flags, const Color* original) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const ID id = HashStr(label, w->idStack.back());
    const bool hasAlpha = !(flags & ColorEditFlags::NoAlpha);
    const UiStyle& style = g.style;

    // ---- Current HSV, keeping the picker's own hue/sat where RGB cannot express them.
    float r = float(*color & 0xFF) / 255.0f, gg = float((*color >> 8) & 0xFF) / 255.0f,
          b = float((*color >> 16) & 0xFF) / 255.0f;
    const uint32_t alpha = ColorAlpha(*color);
    float H, S, V;
    RgbToHsv(r, gg, b, H, S, V);
    if (g.colorPickerId == id) {
        if (S == 0.0f) H = g.colorPickerHue;
        if (V == 0.0f) {
            H = g.colorPickerHue;
            S = g.colorPickerSat;
        }
    }

    // ---- Geometry
    const float barW = std::floor(style.frameRounding + g.fontSize * 0.9f);
    const float spacing = style.itemInnerSpacing.x;
    const float width = ui_detail::CalcItemWidth(g);
    const float svSize = std::max(32.0f, width - (barW + spacing) * (hasAlpha ? 2.0f : 1.0f));
    const Vec2 origin = w->cursorPos;
    const Rect sv(origin, origin + Vec2(svSize, svSize));
    const Rect hue(sv.max.x + spacing, origin.y, sv.max.x + spacing + barW, origin.y + svSize);
    const Rect alphaBar(hue.max.x + spacing, origin.y, hue.max.x + spacing + barW, origin.y + svSize);
    Rect block(origin, {hasAlpha ? alphaBar.max.x : hue.max.x, origin.y + svSize});
    // "Current" and "Original" swatches in a column beside the bars.
    const bool sidePreview = !(flags & ColorEditFlags::NoSidePreview);
    Rect side;
    if (sidePreview) {
        const float sideW = std::floor(std::max(g.fontSize * 3.4f, TextSize(g, "Original").x));
        side = Rect(block.max.x + style.itemInnerSpacing.x * 2.0f, origin.y,
                    block.max.x + style.itemInnerSpacing.x * 2.0f + sideW, origin.y + svSize);
        block.max.x = side.max.x;
    }
    ItemSize(g, block.Size());
    const bool added = ItemAdd(g, block, id);

    bool changed = false;
    auto drag = [&](ID rid, const Rect& rect, float& nx, float& ny) {
        bool hovered, held;
        ButtonBehavior(g, rect, rid, &hovered, &held);
        if (held) {
            nx = Clamp((g.mousePos.x - rect.min.x) / std::max(rect.Width(), 1.0f), 0.0f, 1.0f);
            ny = Clamp((g.mousePos.y - rect.min.y) / std::max(rect.Height(), 1.0f), 0.0f, 1.0f);
            changed = true;
        }
    };
    if (added) {
        float sx = S, sy = 1.0f - V, dummy = 0.0f;
        drag(HashStr("#sv", id), sv, sx, sy);
        if (changed) {
            S = sx;
            V = 1.0f - sy;
        }
        bool hueChanged = false;
        {
            const bool before = changed;
            changed = false;
            drag(HashStr("#hue", id), hue, dummy, H);
            hueChanged = changed;
            changed = changed || before;
        }
        (void)hueChanged;
        if (hasAlpha) {
            float ay = 1.0f - float(alpha) / 255.0f;
            const bool before = changed;
            changed = false;
            drag(HashStr("#alpha", id), alphaBar, dummy, ay);
            if (changed) {
                const uint32_t a = uint32_t(Clamp(1.0f - ay, 0.0f, 1.0f) * 255.0f + 0.5f);
                *color = WithAlpha(*color, a);
            }
            changed = changed || before;
        }
    }

    g.colorPickerId = id;
    g.colorPickerHue = H;
    g.colorPickerSat = S;
    g.colorPickerVal = V;
    if (changed) {
        const uint32_t a = hasAlpha ? ColorAlpha(*color) : 255;
        *color = ColorFromHsv(H, S, V, a);
        w->lastItemStatus |= kItemEdited;
    }

    // Side preview: the current color, and the original one (clicking it restores it).
    const float lineH = TextLineHeight(g);
    const float swatchH = std::floor(std::min(g.fontSize * 2.6f, (side.Height() - lineH * 2.0f - style.itemInnerSpacing.y * 3.0f) * 0.5f));
    const Rect currentSwatch(side.min.x, side.min.y + lineH + style.itemInnerSpacing.y, side.max.x,
                             side.min.y + lineH + style.itemInnerSpacing.y + swatchH);
    const Rect originalSwatch(side.min.x, currentSwatch.max.y + style.itemInnerSpacing.y * 2.0f + lineH, side.max.x,
                              currentSwatch.max.y + style.itemInnerSpacing.y * 2.0f + lineH + swatchH);
    bool originalHovered = false;
    if (added && sidePreview && original && swatchH > 4.0f) {
        bool held;
        if (ButtonBehavior(g, originalSwatch, HashStr("#original", id), &originalHovered, &held) && *color != *original) {
            *color = *original;
            RgbToHsv(float(*color & 0xFF) / 255.0f, float((*color >> 8) & 0xFF) / 255.0f,
                     float((*color >> 16) & 0xFF) / 255.0f, g.colorPickerHue, g.colorPickerSat, g.colorPickerVal);
            H = g.colorPickerHue;
            S = g.colorPickerSat;
            V = g.colorPickerVal;
            changed = true;
            w->lastItemStatus |= kItemEdited;
        }
        if (originalHovered) SetTooltip("Click to restore the original color");
    }

    // ---- Draw
    if (added) {
        DrawList& dl = *w->dl;
        const Color hueFull = ColorFromHsv(H, 1.0f, 1.0f, 255);
        dl.AddRectFilledMultiColor(sv, colors::White, hueFull, colors::Black, colors::Black);
        // SV cursor.
        const Vec2 svCur(sv.min.x + S * svSize, sv.min.y + (1.0f - V) * svSize);
        dl.AddCircle(svCur, 5.0f, colors::Black, 1.5f);
        dl.AddCircle(svCur, 4.0f, colors::White, 1.5f);
        // Hue bar: six gradient segments.
        static const Color kHue[7] = {Hex(0xFF0000), Hex(0xFFFF00), Hex(0x00FF00), Hex(0x00FFFF),
                                      Hex(0x0000FF), Hex(0xFF00FF), Hex(0xFF0000)};
        const float seg = hue.Height() / 6.0f;
        for (int i = 0; i < 6; ++i) {
            dl.AddRectGradient(Rect(hue.min.x, hue.min.y + seg * float(i), hue.max.x, hue.min.y + seg * float(i + 1)),
                               kHue[i], kHue[i + 1], Gradient::Vertical);
        }
        const float hueY = hue.min.y + H * hue.Height();
        dl.AddRectFilled(Rect(hue.min.x - 1.0f, hueY - 2.0f, hue.max.x + 1.0f, hueY + 2.0f), colors::White);
        dl.AddRect(Rect(hue.min.x - 1.0f, hueY - 2.0f, hue.max.x + 1.0f, hueY + 2.0f), colors::Black, 0.0f, 1.0f);
        // Alpha bar.
        if (hasAlpha) {
            DrawChecker(dl, alphaBar, 0.0f);
            const Color opaque = WithAlpha(*color, 255);
            dl.AddRectGradient(alphaBar, opaque, WithAlpha(opaque, 0), Gradient::Vertical);
            const float ay = alphaBar.min.y + (1.0f - float(ColorAlpha(*color)) / 255.0f) * alphaBar.Height();
            dl.AddRectFilled(Rect(alphaBar.min.x - 1.0f, ay - 2.0f, alphaBar.max.x + 1.0f, ay + 2.0f), colors::White);
            dl.AddRect(Rect(alphaBar.min.x - 1.0f, ay - 2.0f, alphaBar.max.x + 1.0f, ay + 2.0f), colors::Black, 0.0f, 1.0f);
        }
        if (sidePreview && swatchH > 4.0f) {
            const float sr = std::min(style.frameRounding, swatchH * 0.5f);
            const Color textColor = StyleColor(g, UiColor::TextDisabled);
            RenderText(g, side.min, "Current", textColor);
            DrawSwatch(g, currentSwatch, *color, sr, hasAlpha);
            dl.AddRect(currentSwatch, StyleColor(g, UiColor::Border), sr, 1.0f);
            if (original) {
                RenderText(g, {side.min.x, currentSwatch.max.y + style.itemInnerSpacing.y}, "Original", textColor);
                DrawSwatch(g, originalSwatch, *original, sr, hasAlpha);
                dl.AddRect(originalSwatch, originalHovered ? StyleColor(g, UiColor::Text, 0.8f) : StyleColor(g, UiColor::Border),
                           sr, 1.0f);
            }
        }
    }

    // ---- RGBA and hex boxes below the picker, as wide as it.
    if (!(flags & ColorEditFlags::NoInputs)) {
        const float fullW = block.Width();
        const uint32_t rowFlags = (flags & (ColorEditFlags::NoAlpha | ColorEditFlags::NoLabel)) | ColorEditFlags::NoPicker |
                                  ColorEditFlags::NoSwatch;
        PushID(label);
        SetNextItemWidth(fullW);
        if (ColorEdit4("##rgba", color, rowFlags | ColorEditFlags::NoLabel)) changed = true;
        SetNextItemWidth(fullW);
        if (ColorEdit4("##hex", color, rowFlags | ColorEditFlags::NoLabel | ColorEditFlags::DisplayHex)) changed = true;
        PopID();
    }
    if (!(flags & ColorEditFlags::NoLabel)) {
        const std::string_view text = VisibleText(label);
        if (!text.empty()) Text(text);
    }
    if (changed) w->lastItemStatus |= kItemEdited;
    return changed;
}

bool Ui::ColorEdit4(std::string_view label, Color* color, uint32_t flags) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const std::string_view text = VisibleText(label);
    const bool hasAlpha = !(flags & ColorEditFlags::NoAlpha);
    bool changed = false;
    BeginGroup();
    PushID(label);

    const bool showInputs = !(flags & ColorEditFlags::NoInputs);
    if (showInputs) {
        const int components = hasAlpha ? 4 : 3;
        const float full = ui_detail::CalcItemWidth(g);
        // Without a swatch the boxes fill the width exactly (the last one takes the rounding remainder).
        const bool noSwatch = (flags & ColorEditFlags::NoSwatch) != 0;
        const float spacing = g.style.itemInnerSpacing.x;
        const float each = std::floor((full - spacing * float(noSwatch ? components - 1 : components)) / float(components));
        const float last = noSwatch ? full - (each + spacing) * float(components - 1) : each;
        if (flags & ColorEditFlags::DisplayHex) {
            char hex[16];
            if (hasAlpha) std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X", *color & 0xFF, (*color >> 8) & 0xFF,
                                        (*color >> 16) & 0xFF, ColorAlpha(*color));
            else std::snprintf(hex, sizeof(hex), "#%02X%02X%02X", *color & 0xFF, (*color >> 8) & 0xFF,
                               (*color >> 16) & 0xFF);
            SetNextItemWidth(full);
            if (InputText("##hex", hex, sizeof(hex))) {
                auto nibble = [](char c) {
                    return (c >= '0' && c <= '9') ? c - '0'
                         : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                         : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
                };
                const char* p = hex[0] == '#' ? hex + 1 : hex;
                int len = 0;
                while (p[len]) ++len;
                const int need = (hasAlpha && len >= 8) ? 4 : 3;
                int v[4] = {0, 0, 0, 255};
                bool ok = len >= 6;
                for (int i = 0; i < need && ok; ++i) {
                    const int hi = nibble(p[i * 2]), lo = nibble(p[i * 2 + 1]);
                    if (hi < 0 || lo < 0) ok = false;
                    else v[i] = hi * 16 + lo;
                }
                if (ok) {
                    *color = Rgba(uint32_t(v[0]), uint32_t(v[1]), uint32_t(v[2]), hasAlpha ? uint32_t(v[3]) : 255);
                    changed = true;
                }
            }
        } else {
            int comp[4] = {int(*color & 0xFF), int((*color >> 8) & 0xFF), int((*color >> 16) & 0xFF),
                           int(ColorAlpha(*color))};
            static const char* const names[4] = {"##R", "##G", "##B", "##A"};
            for (int i = 0; i < components; ++i) {
                if (i > 0) SameLine(0.0f, spacing);
                SetNextItemWidth(i + 1 < components ? each : last);
                if (DragInt(names[i], &comp[i], 0.5f, 0, 255)) changed = true;
            }
            if (changed) *color = Rgba(uint32_t(comp[0]), uint32_t(comp[1]), uint32_t(comp[2]),
                                       hasAlpha ? uint32_t(comp[3]) : 255);
        }
    }

    // The swatch: a button that opens the picker in a popup.
    Rect swatch;
    if (!(flags & ColorEditFlags::NoSwatch)) {
        if (showInputs) SameLine(0.0f, g.style.itemInnerSpacing.x);
        const float h = FrameHeight(g);
        const bool clicked = ColorButton("##swatch", *color, {showInputs ? h : std::floor(h * 1.6f), h}, flags);
        swatch = w->lastItemRect;
        if (clicked && !(flags & ColorEditFlags::NoPicker)) {
            g.colorPickerOriginal = *color;
            OpenPopup("##picker");
        }
        if (!(flags & ColorEditFlags::NoTooltip) && IsItemHovered() && !IsPopupOpen("##picker")) {
            BeginTooltip();
            if (!text.empty()) Text(text);
            ColorButton("##tip", *color, {g.fontSize * 3.0f, g.fontSize * 3.0f}, flags & ColorEditFlags::NoAlpha);
            SameLine();
            const Color c = *color;
            TextF("#%02X%02X%02X%02X\nR %u  G %u  B %u  A %u", c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24,
                  c & 0xFFu, (c >> 8) & 0xFFu, (c >> 16) & 0xFFu, c >> 24);
            EndTooltip();
        }
    }
    if (!(flags & ColorEditFlags::NoLabel) && !text.empty()) {
        SameLine(0.0f, g.style.itemInnerSpacing.x);
        AlignTextToFramePadding();
        Text(text);
    }
    if (swatch.Width() > 0.0f && IsPopupOpen("##picker")) {
        // Open below the swatch (above it when there is no room), like a combo box, rather than at the mouse.
        g.nextWindow.hasPos = true;
        g.nextWindow.pos = {swatch.min.x, swatch.max.y + g.style.itemInnerSpacing.y};
        g.nextWindow.posCond = Cond::Appearing;
        g.nextWindow.pivot = {};
        g.nextWindow.hasAnchor = true;
        g.nextWindow.anchor = swatch;
    }
    if (BeginPopup("##picker")) {
        if (!text.empty()) {
            TextDisabled(text);
            Separator();
        }
        SetNextItemWidth(std::floor(g.fontSize * 11.0f));
        if (ColorPicker4("##pick", color, (flags & ColorEditFlags::NoAlpha) | ColorEditFlags::NoLabel,
                         &g.colorPickerOriginal)) {
            changed = true;
        }
        EndPopup();
    }
    PopID();
    EndGroup();
    if (changed) w->lastItemStatus |= kItemEdited;
    return changed;
}

bool Ui::ColorEdit3(std::string_view label, Color* color, uint32_t flags) {
    return ColorEdit4(label, color, flags | ColorEditFlags::NoAlpha);
}

} // namespace drizzy
