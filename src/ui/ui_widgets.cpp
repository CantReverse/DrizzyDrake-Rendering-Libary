// UI widgets: text, buttons, toggles, sliders, drags, combos, selectables, trees, tabs, plots.
#include "ui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace drizzy {
using namespace ui_detail;

namespace {

UiColor FrameColor(bool hovered, bool held) {
    return held ? UiColor::FrameBgActive : hovered ? UiColor::FrameBgHovered : UiColor::FrameBg;
}

// Label text drawn to the right of a framed widget.
void RenderLabelRight(UiState& g, const Rect& frame, std::string_view label) {
    if (label.empty()) return;
    RenderText(g, {frame.max.x + g.style.itemInnerSpacing.x, frame.min.y + g.style.framePadding.y}, label,
               StyleColor(g, UiColor::Text));
}

// Frame plus optional label to its right; returns the total bounding box.
Rect FramedItemBounds(const UiState& g, const Rect& frame, float labelWidth) {
    return {frame.min, {frame.max.x + (labelWidth > 0.0f ? g.style.itemInnerSpacing.x + labelWidth : 0.0f),
                        frame.max.y}};
}

bool ButtonEx(UiState& g, std::string_view label, Vec2 sizeArg, uint32_t flags) {
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Vec2 textSize = TextSize(g, text);
    const Vec2 pad = g.style.framePadding;
    const Vec2 size = CalcItemSize(g, sizeArg, textSize.x + pad.x * 2.0f, TextLineHeight(g) + pad.y * 2.0f);
    Vec2 pos = w->cursorPos;
    if ((flags & kButtonAlignTextBaseLine) && pad.y < w->currLineTextBaseOffset) {
        pos.y += w->currLineTextBaseOffset - pad.y;
    }
    const Rect bb = Rect::FromPosSize(pos, size);
    ItemSize(g, size, pad.y);
    if (!ItemAdd(g, bb, id)) return false;
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, bb, id, &hovered, &held, flags);
    const UiColor color = held && hovered ? UiColor::ButtonActive : hovered ? UiColor::ButtonHovered : UiColor::Button;
    RenderFrame(g, bb, StyleColor(g, color), true, g.style.frameRounding);
    const Rect inner(bb.min + pad, bb.max - pad);
    RenderTextAligned(g, inner, text, g.style.buttonTextAlign, &bb, &textSize);
    return pressed;
}

bool SliderImpl(UiState& g, std::string_view label, float* valueF, int* valueI, float vmin, float vmax,
                const char* format) {
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const float width = ui_detail::CalcItemWidth(g);
    const Rect frame = Rect::FromPosSize(w->cursorPos, {width, FrameHeight(g)});
    const Rect total = FramedItemBounds(g, frame, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return false;

    const bool hovered = ItemHoverable(g, frame, id);
    if (hovered && g.mouseClicked[0]) SetActiveID(g, id, w);
    const bool isInt = valueI != nullptr;
    const float range = vmax - vmin;
    float value = isInt ? float(*valueI) : *valueF;
    float grabW = g.style.grabMinSize;
    if (isInt && range > 0.0f) grabW = std::max(grabW, (width - 4.0f) / (range + 1.0f));
    grabW = std::min(grabW, std::max(width - 4.0f, 1.0f));
    const float trackMin = frame.min.x + 2.0f + grabW * 0.5f, trackMax = frame.max.x - 2.0f - grabW * 0.5f;

    bool changed = false;
    const bool active = g.activeId == id;
    if (active) {
        if (g.input.mouseDown[0]) {
            const float t = trackMax > trackMin ? Clamp((g.mousePos.x - trackMin) / (trackMax - trackMin), 0.0f, 1.0f) : 0.0f;
            float v = vmin + t * range;
            if (isInt) v = std::round(v);
            if (v != value) {
                value = v;
                changed = true;
                if (isInt) *valueI = int(v);
                else *valueF = v;
            }
        } else {
            ClearActiveID(g);
        }
    }

    DrawList& dl = *w->dl;
    RenderFrame(g, frame, StyleColor(g, FrameColor(hovered, active)), true, g.style.frameRounding);
    const float t = range != 0.0f ? Clamp((value - vmin) / range, 0.0f, 1.0f) : 0.0f;
    const float gx = Lerp(trackMin, trackMax, t);
    dl.AddRectFilled(Rect(gx - grabW * 0.5f, frame.min.y + 2.0f, gx + grabW * 0.5f, frame.max.y - 2.0f),
                     StyleColor(g, active ? UiColor::SliderGrabActive : UiColor::SliderGrab), g.style.grabRounding);
    const char* valueText = isInt ? FormatNumber(g, format, *valueI) : FormatNumber(g, format, double(*valueF));
    RenderTextAligned(g, frame, valueText, {0.5f, 0.5f});
    RenderLabelRight(g, frame, text);
    if (changed) w->lastItemStatus |= kItemEdited;
    return changed;
}

bool DragImpl(UiState& g, std::string_view label, float* valueF, int* valueI, float speed, float vmin, float vmax,
              const char* format) {
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Rect frame = Rect::FromPosSize(w->cursorPos, {ui_detail::CalcItemWidth(g), FrameHeight(g)});
    const Rect total = FramedItemBounds(g, frame, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return false;

    const bool hovered = ItemHoverable(g, frame, id);
    if (hovered && g.mouseClicked[0]) {
        SetActiveID(g, id, w);
        g.dragAccumulator = 0.0f;
    }
    const bool active = g.activeId == id;
    if (hovered || active) g.cursorRequest = MouseCursor::ResizeEW;
    bool changed = false;
    if (active) {
        if (g.input.mouseDown[0]) {
            const float scale = g.input.keyShift ? 10.0f : g.input.keyAlt ? 0.1f : 1.0f;
            g.dragAccumulator += g.mouseDelta.x * speed * scale;
            const bool clamp = vmin < vmax;
            if (valueI) {
                const int steps = int(g.dragAccumulator);
                if (steps != 0) {
                    g.dragAccumulator -= float(steps);
                    int v = *valueI + steps;
                    if (clamp) v = std::min(std::max(v, int(vmin)), int(vmax));
                    changed = v != *valueI;
                    *valueI = v;
                }
            } else if (g.dragAccumulator != 0.0f) {
                float v = *valueF + g.dragAccumulator;
                g.dragAccumulator = 0.0f;
                if (clamp) v = Clamp(v, vmin, vmax);
                changed = v != *valueF;
                *valueF = v;
            }
        } else {
            ClearActiveID(g);
        }
    }
    RenderFrame(g, frame, StyleColor(g, FrameColor(hovered, active)), true, g.style.frameRounding);
    const char* valueText = valueI ? FormatNumber(g, format, *valueI) : FormatNumber(g, format, double(*valueF));
    RenderTextAligned(g, frame, valueText, {0.5f, 0.5f});
    RenderLabelRight(g, frame, text);
    if (changed) w->lastItemStatus |= kItemEdited;
    return changed;
}

bool TreeNodeImpl(UiState& g, ID id, std::string_view text, uint32_t flags, bool framed) {
    Window* w = g.current;
    const bool leaf = (flags & TreeNodeFlags::Leaf) != 0;
    bool open = leaf || w->storage.GetInt(id, (flags & TreeNodeFlags::DefaultOpen) ? 1 : 0) != 0;
    if (g.nextItem.hasOpen) {
        if (g.nextItem.openCond == Cond::Always || !w->storage.Has(id)) {
            open = g.nextItem.open;
            w->storage.SetInt(id, open ? 1 : 0);
        }
        g.nextItem.hasOpen = false;
    }
    const float height = framed ? FrameHeight(g) : TextLineHeight(g) + 2.0f;
    const float textOffsetY = framed ? g.style.framePadding.y : 1.0f;
    const Vec2 pos = w->cursorPos;
    const Rect bb(pos, {std::max(w->contentRegionMax.x, pos.x + 1.0f), pos.y + height});
    ItemSize(g, bb.Size(), textOffsetY);
    if (ItemAdd(g, bb, id)) {
        bool hovered, held;
        if (ButtonBehavior(g, bb, id, &hovered, &held) && !leaf) {
            open = !open;
            w->storage.SetInt(id, open ? 1 : 0);
        }
        DrawList& dl = *w->dl;
        const UiColor color = held && hovered ? UiColor::HeaderActive : hovered ? UiColor::HeaderHovered : UiColor::Header;
        if (framed) {
            RenderFrame(g, bb, StyleColor(g, color), true, g.style.frameRounding);
        } else if (hovered) {
            dl.AddRectFilled(bb, StyleColor(g, color, held ? 1.0f : 0.6f), g.style.frameRounding * 0.5f);
        }
        const float padX = framed ? g.style.framePadding.x : 0.0f;
        const Vec2 arrowCenter(pos.x + padX + g.fontSize * 0.5f, pos.y + height * 0.5f);
        if (leaf) {
            dl.AddCircleFilled(arrowCenter, g.fontSize * 0.15f, StyleColor(g, UiColor::Text));
        } else {
            RenderArrow(g, arrowCenter, g.fontSize * 0.55f, open ? 1 : 0, StyleColor(g, UiColor::Text));
        }
        RenderText(g, {pos.x + padX + g.fontSize + g.style.itemInnerSpacing.x, pos.y + textOffsetY}, text,
                   StyleColor(g, UiColor::Text));
    }
    return open;
}

void PlotImpl(Ui& ui, UiState& g, std::string_view label, const float* values, int count, int offset,
              std::string_view overlay, float scaleMin, float scaleMax, Vec2 sizeArg, bool histogram) {
    Window* w = g.current;
    if (w->skipItems) return;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Vec2 size = CalcItemSize(g, sizeArg, ui_detail::CalcItemWidth(g), FrameHeight(g) * 2.5f);
    const Rect frame = Rect::FromPosSize(w->cursorPos, size);
    const Rect total = FramedItemBounds(g, frame, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return;
    RenderFrame(g, frame, StyleColor(g, UiColor::FrameBg), true, g.style.frameRounding);
    RenderLabelRight(g, frame, text);
    if (count <= 0 || !values) return;

    if (scaleMin == FLT_MAX || scaleMax == FLT_MAX) {
        float lo = FLT_MAX, hi = -FLT_MAX;
        for (int i = 0; i < count; ++i) {
            lo = std::min(lo, values[i]);
            hi = std::max(hi, values[i]);
        }
        if (scaleMin == FLT_MAX) scaleMin = lo;
        if (scaleMax == FLT_MAX) scaleMax = hi;
    }
    const float range = scaleMax - scaleMin;
    const Rect inner(frame.min + g.style.framePadding, frame.max - g.style.framePadding);
    auto valueAt = [&](int i) { return values[(i + offset) % count]; };
    auto normalized = [&](float v) { return range != 0.0f ? Clamp((v - scaleMin) / range, 0.0f, 1.0f) : 0.5f; };
    DrawList& dl = *w->dl;

    int hoveredIndex = -1;
    const bool hovered = ItemHoverable(g, frame, id) && inner.Contains(g.mousePos);
    if (hovered) {
        const float t = (g.mousePos.x - inner.min.x) / std::max(inner.Width(), 1.0f);
        hoveredIndex = std::min(count - 1, std::max(0, int(t * float(histogram ? count : count - 1) + (histogram ? 0.0f : 0.5f))));
    }
    if (histogram) {
        const float barW = inner.Width() / float(count);
        for (int i = 0; i < count; ++i) {
            const float x0 = inner.min.x + barW * float(i);
            const float top = inner.max.y - normalized(valueAt(i)) * inner.Height();
            dl.AddRectFilled(Rect(x0, top, x0 + std::max(barW - 1.0f, 1.0f), inner.max.y),
                             StyleColor(g, UiColor::PlotHistogram, i == hoveredIndex ? 1.0f : 0.85f));
        }
    } else if (count >= 2) {
        g.scratchPoints.resize(size_t(count));
        for (int i = 0; i < count; ++i) {
            g.scratchPoints[size_t(i)] = {inner.min.x + inner.Width() * float(i) / float(count - 1),
                                          inner.max.y - normalized(valueAt(i)) * inner.Height()};
        }
        dl.AddPolyline(g.scratchPoints.data(), uint32_t(count), StyleColor(g, UiColor::PlotLines), 1.5f);
        if (hoveredIndex >= 0) {
            dl.AddCircleFilled(g.scratchPoints[size_t(hoveredIndex)], 3.0f, StyleColor(g, UiColor::PlotLines));
        }
    }
    if (!overlay.empty()) {
        RenderTextAligned(g, Rect(frame.min.x, frame.min.y + g.style.framePadding.y, frame.max.x, frame.max.y),
                          overlay, {0.5f, 0.0f});
    }
    if (hoveredIndex >= 0) {
        char tip[64];
        std::snprintf(tip, sizeof(tip), "%d: %.4g", hoveredIndex, double(valueAt(hoveredIndex)));
        ui.SetTooltip(tip);
    }
}

} // namespace

// =====================================================================================================================
// Text
// =====================================================================================================================
void Ui::Text(std::string_view text) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const Vec2 pos(w->cursorPos.x, w->cursorPos.y + w->currLineTextBaseOffset);
    const Vec2 size = text.empty() ? Vec2(0.0f, TextLineHeight(g)) : TextSize(g, text);
    ItemSize(g, size);
    if (!ItemAdd(g, Rect::FromPosSize(pos, size), 0)) return;
    RenderText(g, pos, text, StyleColor(g, UiColor::Text));
}

void Ui::TextF(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const char* text = FormatV(*m, format, args);
    va_end(args);
    Text(text);
}

void Ui::TextColored(Color color, std::string_view text) {
    PushStyleColor(UiColor::Text, color);
    Text(text);
    PopStyleColor();
}

void Ui::TextDisabled(std::string_view text) {
    PushStyleColor(UiColor::Text, m->style.colors[size_t(UiColor::TextDisabled)]);
    Text(text);
    PopStyleColor();
}

void Ui::TextWrapped(std::string_view text) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const float wrap = std::max(w->contentRegionMax.x - w->cursorPos.x, 1.0f);
    const Vec2 pos(w->cursorPos.x, w->cursorPos.y + w->currLineTextBaseOffset);
    const Vec2 size = text.empty() ? Vec2(0.0f, TextLineHeight(g)) : TextSize(g, text, wrap);
    ItemSize(g, size);
    if (!ItemAdd(g, Rect::FromPosSize(pos, size), 0)) return;
    RenderText(g, pos, text, StyleColor(g, UiColor::Text), wrap);
}

void Ui::LabelText(std::string_view label, std::string_view value) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const std::string_view text = VisibleText(label);
    const Rect frame = Rect::FromPosSize(w->cursorPos, {ui_detail::CalcItemWidth(g), FrameHeight(g)});
    const Rect total = FramedItemBounds(g, frame, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, 0)) return;
    RenderText(g, {frame.min.x, frame.min.y + g.style.framePadding.y}, value, StyleColor(g, UiColor::Text));
    RenderLabelRight(g, frame, text);
}

void Ui::BulletText(std::string_view text) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const float lineH = TextLineHeight(g);
    const Vec2 pos(w->cursorPos.x, w->cursorPos.y + w->currLineTextBaseOffset);
    const float textX = g.fontSize + g.style.itemInnerSpacing.x;
    const Vec2 size(textX + TextSize(g, text).x, lineH);
    ItemSize(g, size);
    if (!ItemAdd(g, Rect::FromPosSize(pos, size), 0)) return;
    w->dl->AddCircleFilled({pos.x + g.fontSize * 0.5f, pos.y + lineH * 0.5f}, g.fontSize * 0.17f,
                           StyleColor(g, UiColor::Text));
    RenderText(g, {pos.x + textX, pos.y}, text, StyleColor(g, UiColor::Text));
}

void Ui::SeparatorText(std::string_view label) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const std::string_view text = VisibleText(label);
    const Vec2 textSize = TextSize(g, text);
    const Vec2 pos = w->cursorPos;
    const float width = std::max(w->contentRegionMax.x - pos.x, textSize.x);
    const Rect bb(pos, {pos.x + width, pos.y + TextLineHeight(g)});
    ItemSize(g, bb.Size());
    if (!ItemAdd(g, bb, 0)) return;
    const float y = std::floor(bb.Center().y);
    const Color lineColor = StyleColor(g, UiColor::Separator);
    const float pad = g.style.itemSpacing.x;
    w->dl->AddRectFilled(Rect(pos.x, y, pos.x + pad, y + 1.0f), lineColor);
    RenderText(g, {pos.x + pad * 2.0f, pos.y}, text, StyleColor(g, UiColor::Text));
    const float x = pos.x + pad * 3.0f + textSize.x;
    if (x < bb.max.x) w->dl->AddRectFilled(Rect(x, y, bb.max.x, y + 1.0f), lineColor);
}

// =====================================================================================================================
// Buttons and toggles
// =====================================================================================================================
bool Ui::Button(std::string_view label, Vec2 size) { return ButtonEx(*m, label, size, 0); }

bool Ui::SmallButton(std::string_view label) {
    PushStyleVar(UiStyleVar::FramePadding, Vec2(m->style.framePadding.x, 0.0f));
    const bool pressed = ButtonEx(*m, label, {}, kButtonAlignTextBaseLine);
    PopStyleVar();
    return pressed;
}

bool Ui::InvisibleButton(std::string_view id, Vec2 size) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const ID itemId = HashStr(id, w->idStack.back());
    const Vec2 s = CalcItemSize(g, size, 0.0f, 0.0f);
    const Rect bb = Rect::FromPosSize(w->cursorPos, s);
    ItemSize(g, s);
    if (!ItemAdd(g, bb, itemId)) return false;
    return ButtonBehavior(g, bb, itemId, nullptr, nullptr);
}

void Ui::Image(TextureId texture, Vec2 size, Vec2 uv0, Vec2 uv1, Color tint, float rounding) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const Rect bb = Rect::FromPosSize(w->cursorPos, size);
    ItemSize(g, size);
    if (!ItemAdd(g, bb, 0)) return;
    w->dl->AddImage(texture, bb, uv0, uv1, ScaleAlpha(tint, g.alpha), rounding);
}

bool Ui::ImageButton(std::string_view id, TextureId texture, Vec2 size, Vec2 uv0, Vec2 uv1, Color tint) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const ID itemId = HashStr(id, w->idStack.back());
    const Vec2 pad = g.style.framePadding;
    const Rect bb = Rect::FromPosSize(w->cursorPos, size + pad * 2.0f);
    ItemSize(g, bb.Size(), pad.y);
    if (!ItemAdd(g, bb, itemId)) return false;
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, bb, itemId, &hovered, &held);
    const UiColor color = held && hovered ? UiColor::ButtonActive : hovered ? UiColor::ButtonHovered : UiColor::Button;
    RenderFrame(g, bb, StyleColor(g, color), true, g.style.frameRounding);
    w->dl->AddImage(texture, Rect(bb.min + pad, bb.max - pad), uv0, uv1, ScaleAlpha(tint, g.alpha),
                    std::max(g.style.frameRounding - pad.x, 0.0f));
    return pressed;
}

bool Ui::Checkbox(std::string_view label, bool* value) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const float box = FrameHeight(g);
    const Rect check = Rect::FromPosSize(w->cursorPos, {box, box});
    const Rect total = FramedItemBounds(g, check, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return false;
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, total, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        w->lastItemStatus |= kItemEdited;
    }
    RenderFrame(g, check, StyleColor(g, FrameColor(hovered, held)), true, g.style.frameRounding);
    if (*value) {
        const float pad = std::max(1.0f, std::floor(box / 5.0f));
        RenderCheckMark(g, check.min + Vec2(pad, pad), StyleColor(g, UiColor::CheckMark), box - pad * 2.0f);
    }
    RenderLabelRight(g, check, text);
    return pressed;
}

bool Ui::RadioButton(std::string_view label, bool active) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const float box = FrameHeight(g);
    const Rect check = Rect::FromPosSize(w->cursorPos, {box, box});
    const Rect total = FramedItemBounds(g, check, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return false;
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, total, id, &hovered, &held);
    const Vec2 center = check.Center();
    const float radius = (box - 1.0f) * 0.5f;
    w->dl->AddCircleFilled(center, radius, StyleColor(g, FrameColor(hovered, held)));
    if (active) {
        const float pad = std::max(1.0f, std::floor(box / 6.0f));
        w->dl->AddCircleFilled(center, radius - pad, StyleColor(g, UiColor::CheckMark));
    }
    RenderLabelRight(g, check, text);
    return pressed;
}

bool Ui::RadioButton(std::string_view label, int* value, int buttonValue) {
    const bool pressed = RadioButton(label, *value == buttonValue);
    if (pressed && *value != buttonValue) {
        *value = buttonValue;
        m->current->lastItemStatus |= kItemEdited;
    }
    return pressed;
}

void Ui::ProgressBar(float fraction, Vec2 sizeArg, std::string_view overlay) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const Vec2 size = CalcItemSize(g, sizeArg, ui_detail::CalcItemWidth(g), FrameHeight(g));
    const Rect bb = Rect::FromPosSize(w->cursorPos, size);
    ItemSize(g, size, g.style.framePadding.y);
    if (!ItemAdd(g, bb, 0)) return;
    RenderFrame(g, bb, StyleColor(g, UiColor::FrameBg), true, g.style.frameRounding);
    if (fraction < 0.0f) {
        // Indeterminate: a segment sweeping across.
        const float t = std::fmod(float(g.time) * 0.9f, 1.4f) - 0.4f;
        const float x0 = bb.min.x + bb.Width() * std::max(t, 0.0f);
        const float x1 = bb.min.x + bb.Width() * std::min(t + 0.4f, 1.0f);
        if (x1 > x0) {
            w->dl->AddRectFilled(Rect(x0, bb.min.y, x1, bb.max.y), StyleColor(g, UiColor::PlotHistogram),
                                 g.style.frameRounding);
        }
        if (!overlay.empty()) RenderTextAligned(g, bb, overlay, {0.5f, 0.5f});
        return;
    }
    fraction = Clamp(fraction, 0.0f, 1.0f);
    if (fraction > 0.0f) {
        const Rect fill(bb.min, {bb.min.x + std::max(bb.Width() * fraction, 1.0f), bb.max.y});
        w->dl->AddRectFilled(fill, StyleColor(g, UiColor::PlotHistogram), g.style.frameRounding);
    }
    const std::string_view text = overlay.empty() ? std::string_view(FormatNumber(g, "%.0f%%", double(fraction * 100.0f)))
                                                  : overlay;
    RenderTextAligned(g, bb, text, {0.5f, 0.5f});
}

void Ui::Bullet() {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const float lineH = std::max(TextLineHeight(g), w->currLineHeight);
    const Rect bb = Rect::FromPosSize(w->cursorPos, {g.fontSize, lineH});
    ItemSize(g, bb.Size());
    if (ItemAdd(g, bb, 0)) {
        w->dl->AddCircleFilled(bb.Center(), g.fontSize * 0.17f, StyleColor(g, UiColor::Text));
    }
    SameLine(0.0f, g.style.framePadding.x);
}

// =====================================================================================================================
// Sliders and drags
// =====================================================================================================================
bool Ui::SliderFloat(std::string_view label, float* value, float min, float max, const char* format) {
    return SliderImpl(*m, label, value, nullptr, min, max, format);
}

bool Ui::SliderInt(std::string_view label, int* value, int min, int max, const char* format) {
    return SliderImpl(*m, label, nullptr, value, float(min), float(max), format);
}

bool Ui::DragFloat(std::string_view label, float* value, float speed, float min, float max, const char* format) {
    return DragImpl(*m, label, value, nullptr, speed, min, max, format);
}

bool Ui::DragInt(std::string_view label, int* value, float speed, int min, int max, const char* format) {
    return DragImpl(*m, label, nullptr, value, speed, float(min), float(max), format);
}

// =====================================================================================================================
// Combos, lists, menus
// =====================================================================================================================
bool Ui::BeginCombo(std::string_view label, std::string_view preview, int maxVisibleItems) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const float height = FrameHeight(g);
    const Rect frame = Rect::FromPosSize(w->cursorPos, {ui_detail::CalcItemWidth(g), height});
    const Rect total = FramedItemBounds(g, frame, TextSize(g, text).x);
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return false;
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, frame, id, &hovered, &held, kButtonPressOnClick);
    if (pressed && !IsPopupOpenId(g, id) && g.popupClosedByClick != id) OpenPopupEx(g, id);
    const bool open = IsPopupOpenId(g, id);

    DrawList& dl = *w->dl;
    const float r = g.style.frameRounding;
    RenderFrame(g, frame, StyleColor(g, FrameColor(hovered, open)), true, r);
    const Rect arrowBox(frame.max.x - height, frame.min.y, frame.max.x, frame.max.y);
    dl.AddRectFilled(arrowBox, StyleColor(g, hovered || open ? UiColor::ButtonHovered : UiColor::Button),
                     CornerRadii(0.0f, r, r, 0.0f));
    RenderArrow(g, arrowBox.Center(), g.fontSize * 0.5f, 1, StyleColor(g, UiColor::Text));
    const Rect previewRect(frame.min + g.style.framePadding,
                           {arrowBox.min.x - g.style.framePadding.x, frame.max.y - g.style.framePadding.y});
    RenderTextAligned(g, previewRect, preview, {0.0f, 0.0f}, &previewRect);
    RenderLabelRight(g, frame, text);
    if (!open) return false;

    g.nextWindow.hasPos = true;
    g.nextWindow.pos = {frame.min.x, frame.max.y};
    g.nextWindow.posCond = Cond::Always;
    g.nextWindow.pivot = {};
    g.nextWindow.hasSize = true;
    g.nextWindow.size = {frame.Width(), 0.0f};
    g.nextWindow.sizeCond = Cond::Always;
    g.nextWindow.hasAnchor = true;
    g.nextWindow.anchor = frame;
    g.nextWindow.maxHeight = float(std::max(maxVisibleItems, 1)) * (TextLineHeight(g) + g.style.itemSpacing.y) +
                             g.style.windowPadding.y * 2.0f;
    return BeginPopupEx(*this, g, id, kWindowCombo | WindowFlags::AlwaysAutoResize);
}

void Ui::EndCombo() { EndPopup(); }

bool Ui::Combo(std::string_view label, int* current, const char* const items[], int count, int maxVisibleItems) {
    const char* preview = (*current >= 0 && *current < count) ? items[*current] : "";
    bool changed = false;
    if (BeginCombo(label, preview, maxVisibleItems)) {
        for (int i = 0; i < count; ++i) {
            PushID(i);
            if (Selectable(items[i], i == *current) && i != *current) {
                *current = i;
                changed = true;
            }
            PopID();
        }
        EndCombo();
    }
    if (changed) m->current->lastItemStatus |= kItemEdited;
    return changed;
}

bool Ui::Selectable(std::string_view label, bool selected, uint32_t flags, Vec2 sizeArg) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Vec2 pos = w->cursorPos;
    const float width = sizeArg.x > 0.0f ? sizeArg.x : std::max(w->contentRegionMax.x - pos.x, TextSize(g, text).x);
    const float height = sizeArg.y > 0.0f ? sizeArg.y : TextLineHeight(g);
    ItemSize(g, {width, height});
    // In a table, SpanAllColumns makes the row the item: its highlight goes under every cell of the row and clicks
    // anywhere on the row hit it.
    TableState* table = (flags & SelectableFlags::SpanAllColumns) ? CurrentTable(g) : nullptr;
    if (table && table->column < 0) table = nullptr;
    // The highlight covers half the item spacing above and below so consecutive rows touch.
    const float spacingAbove = std::floor(g.style.itemSpacing.y * 0.5f);
    const Rect bb = table ? Rect(table->x0, table->rowTop, table->x1,
                                 table->rowTop + std::max(table->rowMinHeight, height + g.style.cellPadding.y * 2.0f))
                          : Rect(pos.x, pos.y - spacingAbove, pos.x + width,
                                 pos.y + height + g.style.itemSpacing.y - spacingAbove);
    const Rect cellClip = w->clipRect;
    if (table) w->clipRect = table->bodyClip;
    const bool visible = ItemAdd(g, bb, id);
    const bool disabled = (flags & SelectableFlags::Disabled) != 0;
    bool hovered = false, held = false, pressed = false;
    if (visible) {
        if (disabled) BeginDisabled();
        pressed = ButtonBehavior(g, bb, id, &hovered, &held);
    }
    w->clipRect = cellClip;
    if (!visible) return false;
    if (hovered || held || selected) {
        const UiColor color = held && hovered ? UiColor::HeaderActive : hovered ? UiColor::HeaderHovered : UiColor::Header;
        if (table) {
            Rect row = bb;
            row.min.y = std::max(row.min.y, table->bodyClip.min.y);
            if (row.max.y > row.min.y) table->mainList->AddRectFilled(row, StyleColor(g, color));
        } else {
            w->dl->AddRectFilled(bb, StyleColor(g, color), std::min(g.style.frameRounding, 4.0f));
        }
    }
    RenderText(g, pos, text, StyleColor(g, UiColor::Text));
    if (disabled) EndDisabled();
    if (pressed && !(flags & SelectableFlags::DontClosePopups) && (w->root->flags & kWindowPopup) &&
        !(w->root->flags & kWindowModal)) {
        CloseCurrentPopup();
    }
    return pressed;
}

bool Ui::Selectable(std::string_view label, bool* selected, uint32_t flags, Vec2 size) {
    if (Selectable(label, *selected, flags, size)) {
        *selected = !*selected;
        return true;
    }
    return false;
}

bool Ui::BeginListBox(std::string_view label, Vec2 size) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const std::string_view text = VisibleText(label);
    const Vec2 frameSize = CalcItemSize(g, size, ui_detail::CalcItemWidth(g),
                                        std::floor(TextLineHeight(g) * 7.25f + g.style.framePadding.y * 2.0f));
    // The label sits to the right of the box, aligned with its first row.
    RenderText(g, {w->cursorPos.x + frameSize.x + g.style.itemInnerSpacing.x, w->cursorPos.y + g.style.framePadding.y},
               text, StyleColor(g, UiColor::Text));
    const float labelEnd = w->cursorPos.x + frameSize.x + (text.empty() ? 0.0f : g.style.itemInnerSpacing.x + TextSize(g, text).x);
    w->cursorMaxPos.x = std::max(w->cursorMaxPos.x, labelEnd);
    PushStyleColor(UiColor::ChildBg, m->style.colors[size_t(UiColor::FrameBg)]);
    PushStyleVar(UiStyleVar::WindowPadding, m->style.framePadding);
    const bool visible = BeginChild(label, frameSize, true);
    PopStyleVar();
    PopStyleColor();
    if (!visible) {
        EndChild();
        return false;
    }
    return true;
}

void Ui::EndListBox() { EndChild(); }

bool Ui::MenuItem(std::string_view label, std::string_view shortcut, bool selected, bool enabled) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Vec2 pos = w->cursorPos;
    const float lineH = TextLineHeight(g);
    const float checkW = g.fontSize;  // reserve room on the right for a check mark / shortcut
    const float shortcutW = shortcut.empty() ? 0.0f : TextSize(g, shortcut).x + g.style.itemInnerSpacing.x;
    // Natural width drives a menu popup's auto-fit; the row then stretches to the window width for drawing and clicks.
    const float naturalW = TextSize(g, text).x + checkW + shortcutW + g.style.framePadding.x * 2.0f;
    ItemSize(g, {naturalW, lineH});
    const Rect bb(pos.x, pos.y - std::floor(g.style.itemSpacing.y * 0.5f),
                  std::max(w->contentRegionMax.x, pos.x + naturalW), pos.y + lineH + g.style.itemSpacing.y -
                      std::floor(g.style.itemSpacing.y * 0.5f));
    if (!ItemAdd(g, bb, id)) return false;
    if (!enabled) BeginDisabled();
    bool hovered, held;
    const bool pressed = ButtonBehavior(g, bb, id, &hovered, &held);
    if (hovered || held) {
        w->dl->AddRectFilled(bb, StyleColor(g, held ? UiColor::HeaderActive : UiColor::HeaderHovered),
                             std::min(g.style.frameRounding, 4.0f));
    }
    RenderText(g, {pos.x + g.style.framePadding.x, pos.y}, text, StyleColor(g, UiColor::Text));
    float right = bb.max.x - g.style.framePadding.x;
    if (selected) {
        const float size = g.fontSize * 0.8f;
        right -= size;
        RenderCheckMark(g, {right, pos.y + (lineH - size) * 0.5f}, StyleColor(g, UiColor::CheckMark), size);
        right -= g.style.itemInnerSpacing.x;
    }
    if (!shortcut.empty()) {
        RenderText(g, {right - TextSize(g, shortcut).x, pos.y}, shortcut, StyleColor(g, UiColor::TextDisabled));
    }
    if (!enabled) EndDisabled();
    // Clicking a menu item closes the whole menu chain.
    if (pressed && enabled && (w->root->flags & kWindowPopup) && !(w->root->flags & kWindowModal)) CloseCurrentPopup();
    return pressed && enabled;
}

// =====================================================================================================================
// Trees
// =====================================================================================================================
bool Ui::TreeNode(std::string_view label, uint32_t flags) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const bool open = TreeNodeImpl(g, id, text, flags, false);
    if (open) {
        Indent();
        w->idStack.push_back(id);
        w->treeDepth++;
    }
    return open;
}

void Ui::TreePop() {
    Window* w = m->current;
    DZ_ASSERT(w->treeDepth > 0 && "TreePop() without an open TreeNode()");
    Unindent();
    PopID();
    w->treeDepth--;
}

bool Ui::CollapsingHeader(std::string_view label, uint32_t flags) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    return TreeNodeImpl(g, id, text, flags, true);
}

void Ui::SetNextItemOpen(bool open, Cond cond) {
    m->nextItem.hasOpen = true;
    m->nextItem.open = open;
    m->nextItem.openCond = cond;
}

// =====================================================================================================================
// Tabs
// =====================================================================================================================
bool Ui::BeginTabBar(std::string_view strId) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const ID id = HashStr(strId, w->idStack.back());
    TabBarState& bar = g.tabBars[id];
    bar.id = id;
    bar.window = w;
    if (bar.nextSelected) {
        bar.selected = bar.nextSelected;
        bar.nextSelected = 0;
    }
    bar.selectedSubmitted = false;
    const float height = FrameHeight(g);
    const Vec2 pos = w->cursorPos;
    const float width = std::max(w->contentRegionMax.x - pos.x, 1.0f);
    bar.rowY = pos.y;
    bar.rowHeight = height;
    bar.nextX = bar.startX = pos.x;
    bar.naturalWidth = 0.0f;
    bar.maxX = pos.x + width;
    const Rect row(pos, pos + Vec2(width, height));
    ItemSize(g, row.Size(), g.style.framePadding.y);
    if (ItemAdd(g, row, id)) {
        w->dl->AddRectFilled(Rect(row.min.x, row.max.y - 1.0f, row.max.x, row.max.y), StyleColor(g, UiColor::TabActive));
    }
    g.tabBarStack.push_back(&bar);
    w->idStack.push_back(id);
    return true;
}

void Ui::EndTabBar() {
    UiState& g = *m;
    DZ_ASSERT(!g.tabBarStack.empty() && "EndTabBar() without BeginTabBar()");
    TabBarState* bar = g.tabBarStack.back();
    if (!bar->selectedSubmitted) bar->selected = 0;  // the selected tab is gone: the first tab takes over
    bar->naturalWidthPrev = bar->naturalWidth;
    g.tabBarStack.pop_back();
    PopID();
}

bool Ui::BeginTabItem(std::string_view label) {
    UiState& g = *m;
    DZ_ASSERT(!g.tabBarStack.empty() && "BeginTabItem() outside BeginTabBar()");
    TabBarState& bar = *g.tabBarStack.back();
    Window* w = g.current;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Vec2 textSize = TextSize(g, text);
    // Tabs keep their natural width while they fit; when last frame's tabs overflowed the bar, all shrink by the same
    // factor (labels clip) so every tab stays reachable.
    const float naturalW = textSize.x + g.style.framePadding.x * 2.0f;
    bar.naturalWidth += naturalW + 2.0f;
    const float available = bar.maxX - bar.startX;
    const float fit = bar.naturalWidthPrev > available ? available / bar.naturalWidthPrev : 1.0f;
    const float tabW = std::max(std::floor(naturalW * fit), g.style.framePadding.x * 2.0f + 4.0f);
    const Rect bb(bar.nextX, bar.rowY, bar.nextX + tabW, bar.rowY + bar.rowHeight);
    bar.nextX += tabW + 2.0f;
    if (bar.selected == 0) bar.selected = id;
    const bool selected = bar.selected == id;
    if (selected) bar.selectedSubmitted = true;

    // Tabs sit in the row reserved by BeginTabBar, so they do not move the layout cursor.
    if (ItemAdd(g, bb, id)) {
        bool hovered, held;
        if (ButtonBehavior(g, bb, id, &hovered, &held, kButtonPressOnClick)) bar.nextSelected = id;
        const UiColor color = selected ? UiColor::TabActive : hovered ? UiColor::TabHovered : UiColor::Tab;
        const float r = g.style.tabRounding;
        w->dl->AddRectFilled(Rect(bb.min, {bb.max.x, bb.max.y - (selected ? 0.0f : 1.0f)}), StyleColor(g, color),
                             CornerRadii(r, r, 0.0f, 0.0f));
        RenderTextAligned(g, bb, text, {0.5f, 0.5f}, nullptr, &textSize);
    }
    return selected;
}

void Ui::EndTabItem() {}

// =====================================================================================================================
// Plots
// =====================================================================================================================
void Ui::PlotLines(std::string_view label, const float* values, int count, int offset, std::string_view overlay,
                   float scaleMin, float scaleMax, Vec2 size) {
    PlotImpl(*this, *m, label, values, count, offset, overlay, scaleMin, scaleMax, size, false);
}

void Ui::PlotHistogram(std::string_view label, const float* values, int count, int offset, std::string_view overlay,
                       float scaleMin, float scaleMax, Vec2 size) {
    PlotImpl(*this, *m, label, values, count, offset, overlay, scaleMin, scaleMax, size, true);
}

} // namespace drizzy
