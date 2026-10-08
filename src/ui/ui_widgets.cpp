// UI widgets: text, buttons, toggles, sliders, drags, combos, selectables, trees, tabs, plots.
#include "ui_internal.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace drizzy {
using namespace ui_detail;

namespace {

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

// The background of a bar that shows a value (progress bars, plots): a filled box, or an outline in outlined looks.
void RenderTrack(UiState& g, const Rect& r) {
    if (g.style.frameShape == FrameShape::Outline) RenderFieldFrame(g, r, false, false);
    else RenderFrame(g, r, StyleColor(g, UiColor::FrameBg), true, g.style.frameRounding);
}

// The blocks of a segmented bar (SliderShape::Segments) inside `r`, lit up to fraction `t`. Returns where the lit
// blocks end.
float RenderSegments(UiState& g, const Rect& r, float t, Color lit, Color unlit) {
    const float gap = 2.0f;
    const int count = std::max(4, std::min(32, int((r.Width() + gap) / std::max(r.Height() * 0.55f + gap, 4.0f))));
    const float each = (r.Width() - gap * float(count - 1)) / float(count);
    const int litCount = int(t * float(count) + 0.5f);
    const float litEnd = r.min.x + (each + gap) * float(litCount) - gap;
    if (litCount > 0) RenderGlow(g, Rect(r.min.x, r.min.y, litEnd, r.max.y), 0.0f, 0.6f);
    DrawList& dl = *g.current->dl;
    const float rounding = std::min(g.style.grabRounding, each * 0.5f);
    for (int i = 0; i < count; ++i) {
        const float x = r.min.x + (each + gap) * float(i);
        dl.AddRectFilled(Rect(std::floor(x), r.min.y, std::floor(x + each), r.max.y), i < litCount ? lit : unlit,
                         rounding);
    }
    return litCount > 0 ? litEnd : r.min.x;
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
    const Rect drawn = RenderButtonFrame(g, bb, hovered, held, g.style.frameRounding);
    const Rect inner(drawn.min + pad, drawn.max - pad);
    RenderTextAligned(g, inner, text, g.style.buttonTextAlign, &drawn, &textSize);
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
    const SliderShape shape = g.style.sliderShape;

    // Where the value maps to: the grab block's travel, the rail or segments (which leave room on their right for the
    // value text, as wide as the widest end of the range), or the whole frame.
    float grabW = 0.0f, trackMin, trackMax;
    Rect rail = frame;
    float knobR = 0.0f;
    if (shape == SliderShape::Rail || shape == SliderShape::Segments) {
        float valueW = TextSize(g, isInt ? FormatNumber(g, format, int(vmin)) : FormatNumber(g, format, double(vmin))).x;
        valueW = std::max(valueW, TextSize(g, isInt ? FormatNumber(g, format, int(vmax))
                                                    : FormatNumber(g, format, double(vmax))).x);
        if (shape == SliderShape::Rail) {
            knobR = std::floor(frame.Height() * 0.3f);
            rail.max.x = frame.max.x - valueW - g.style.itemInnerSpacing.x;
        } else {
            rail = Rect(frame.min.x + 3.0f, frame.min.y + 3.0f, frame.max.x - valueW - g.style.framePadding.x * 2.0f,
                        frame.max.y - 3.0f);
        }
        rail.max.x = std::max(rail.max.x, rail.min.x + knobR * 2.0f + 8.0f);
        trackMin = rail.min.x + knobR;
        trackMax = rail.max.x - knobR;
    } else if (shape == SliderShape::Fill) {
        trackMin = frame.min.x;
        trackMax = frame.max.x;
    } else {
        grabW = g.style.grabMinSize;
        if (isInt && range > 0.0f) grabW = std::max(grabW, (width - 4.0f) / (range + 1.0f));
        grabW = std::min(grabW, std::max(width - 4.0f, 1.0f));
        trackMin = frame.min.x + 2.0f + grabW * 0.5f;
        trackMax = frame.max.x - 2.0f - grabW * 0.5f;
    }

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
    const float t = range != 0.0f ? Clamp((value - vmin) / range, 0.0f, 1.0f) : 0.0f;
    const float gx = Lerp(trackMin, trackMax, t);
    const Color grab = StyleColor(g, active ? UiColor::SliderGrabActive : UiColor::SliderGrab);
    const char* valueText = isInt ? FormatNumber(g, format, *valueI) : FormatNumber(g, format, double(*valueF));
    const Rect valueRect(rail.max.x, frame.min.y, frame.max.x - (shape == SliderShape::Segments ? g.style.framePadding.x : 0.0f),
                         frame.max.y);
    switch (shape) {
    case SliderShape::Rail: {
        const float railH = std::max(4.0f, std::floor(frame.Height() * 0.18f));
        const float cy = std::floor(frame.Center().y);
        const Rect track(rail.min.x, cy - railH * 0.5f, rail.max.x, cy + railH * 0.5f);
        dl.AddRectFilled(track, StyleColor(g, hovered || active ? UiColor::FrameBgActive : UiColor::FrameBg), railH * 0.5f);
        if (gx > track.min.x) {
            const Rect filled(track.min.x, track.min.y, gx, track.max.y);
            RenderGlow(g, filled, railH * 0.5f, 0.6f);
            dl.AddRectFilled(filled, StyleColor(g, UiColor::SliderGrab), railH * 0.5f);
        }
        RenderKnob(g, {gx, cy}, knobR, KnobColor(g, grab), active ? 1.0f : 0.7f);
        RenderTextAligned(g, valueRect, valueText, {1.0f, 0.5f});
        break;
    }
    case SliderShape::Segments:
        RenderFieldFrame(g, frame, hovered, active);
        RenderSegments(g, rail, t, grab, StyleColor(g, hovered || active ? UiColor::FrameBgActive : UiColor::FrameBg));
        RenderTextAligned(g, valueRect, valueText, {1.0f, 0.5f});
        break;
    case SliderShape::Fill:
        RenderFieldFrame(g, frame, hovered, active);
        if (gx > frame.min.x + 0.5f) {
            const Rect filled(frame.min.x, frame.min.y, gx, frame.max.y);
            RenderGlow(g, filled, g.style.frameRounding, active ? 0.9f : 0.5f);
            RenderFrame(g, filled, grab, false, g.style.frameRounding);
        }
        RenderTextOverFill(g, frame, valueText, gx);
        break;
    default: {
        RenderFieldFrame(g, frame, hovered, active);
        const Rect grabRect(gx - grabW * 0.5f, frame.min.y + 2.0f, gx + grabW * 0.5f, frame.max.y - 2.0f);
        RenderGlow(g, grabRect, g.style.grabRounding, active ? 1.0f : 0.6f);
        dl.AddRectFilled(grabRect, grab, g.style.grabRounding);
        RenderTextAligned(g, frame, valueText, {0.5f, 0.5f});
        break;
    }
    }
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
    RenderFieldFrame(g, frame, hovered, active);
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
    RenderTrack(g, frame);
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
    const Rect drawn = RenderButtonFrame(g, bb, hovered, held, g.style.frameRounding);
    w->dl->AddImage(texture, Rect(drawn.min + pad, drawn.max - pad), uv0, uv1, ScaleAlpha(tint, g.alpha),
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
    const bool filledFrame = g.style.frameShape == FrameShape::Filled;
    const float rounding = filledFrame ? g.style.frameRounding : std::min(g.style.frameRounding, box * 0.5f);
    const CheckShape shape = g.style.checkShape;
    if (!*value || shape != CheckShape::Fill) RenderCheckBox(g, check, hovered, held, false);
    if (*value) {
        const float pad = std::max(1.0f, std::floor(box / 5.0f));
        if (shape == CheckShape::Fill) {
            RenderGlow(g, check, rounding, hovered ? 1.0f : 0.7f);
            RenderFrame(g, check, StyleColor(g, hovered ? UiColor::SliderGrabActive : UiColor::CheckMark), false, rounding);
            RenderCheckMark(g, check.min + Vec2(pad, pad), StyleColor(g, UiColor::AccentText), box - pad * 2.0f);
        } else if (shape == CheckShape::Square) {
            const float inset = std::max(3.0f, std::floor(box * 0.25f));
            const Rect dot = check.Expanded(-inset);
            RenderGlow(g, dot, std::max(rounding - inset * 0.5f, 0.0f), 0.9f);
            w->dl->AddRectFilled(dot, StyleColor(g, UiColor::CheckMark), std::max(rounding - inset * 0.5f, 0.0f));
        } else {
            RenderGlow(g, check, rounding, 0.6f);
            RenderCheckMark(g, check.min + Vec2(pad, pad), StyleColor(g, UiColor::CheckMark), box - pad * 2.0f);
        }
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
    const CheckShape shape = g.style.checkShape;
    if (!active || shape != CheckShape::Fill) RenderCheckBox(g, check, hovered, held, true);
    if (active) {
        DrawList& dl = *w->dl;
        if (shape == CheckShape::Fill) {
            RenderGlow(g, Rect::FromCenter(center, {radius, radius}), radius, hovered ? 1.0f : 0.7f);
            dl.AddCircleFilled(center, radius, StyleColor(g, hovered ? UiColor::SliderGrabActive : UiColor::CheckMark));
            dl.AddCircleFilled(center, std::max(radius * 0.38f, 2.0f), StyleColor(g, UiColor::AccentText));
        } else {
            const float pad = shape == CheckShape::Square ? std::max(3.0f, std::floor(box * 0.25f))
                                                          : std::max(1.0f, std::floor(box / 6.0f));
            RenderGlow(g, Rect::FromCenter(center, {radius - pad, radius - pad}), radius - pad, 0.9f);
            dl.AddCircleFilled(center, radius - pad, StyleColor(g, UiColor::CheckMark));
        }
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
    RenderTrack(g, bb);
    const Color fillColor = StyleColor(g, UiColor::PlotHistogram);
    const float rounding = g.style.frameRounding;
    if (fraction < 0.0f) {
        // Indeterminate: a segment sweeping across.
        const float t = std::fmod(float(g.time) * 0.9f, 1.4f) - 0.4f;
        const float x0 = bb.min.x + bb.Width() * std::max(t, 0.0f);
        const float x1 = bb.min.x + bb.Width() * std::min(t + 0.4f, 1.0f);
        if (x1 > x0) {
            const Rect fill(x0, bb.min.y, x1, bb.max.y);
            RenderGlow(g, fill, rounding, 0.6f);
            RenderFrame(g, fill, fillColor, false, rounding);
        }
        if (!overlay.empty()) RenderTextAligned(g, bb, overlay, {0.5f, 0.5f});
        return;
    }
    fraction = Clamp(fraction, 0.0f, 1.0f);
    float fillEnd = bb.min.x;
    if (g.style.sliderShape == SliderShape::Segments) {
        fillEnd = RenderSegments(g, bb.Expanded(-3.0f), fraction, fillColor, StyleColor(g, UiColor::FrameBgHovered, 0.6f));
    } else if (fraction > 0.0f) {
        const Rect fill(bb.min, {bb.min.x + std::max(bb.Width() * fraction, 1.0f), bb.max.y});
        RenderGlow(g, fill, rounding, 0.6f);
        RenderFrame(g, fill, fillColor, false, rounding);
        fillEnd = fill.max.x;
    }
    const std::string_view text = overlay.empty() ? std::string_view(FormatNumber(g, "%.0f%%", double(fraction * 100.0f)))
                                                  : overlay;
    RenderTextOverFill(g, bb, text, fillEnd);
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
    RenderFieldFrame(g, frame, hovered, open);
    const Rect arrowBox(frame.max.x - height, frame.min.y, frame.max.x, frame.max.y);
    if (g.style.frameShape == FrameShape::Filled) {
        dl.AddRectFilled(arrowBox, StyleColor(g, hovered || open ? UiColor::ButtonHovered : UiColor::Button),
                         CornerRadii(0.0f, r, r, 0.0f));
    }
    RenderArrow(g, arrowBox.Center(), g.fontSize * 0.5f, open ? 3 : 1,
                StyleColor(g, open && g.style.frameShape != FrameShape::Filled ? UiColor::CheckMark : UiColor::Text));
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
        const Rect line(row.min.x, row.max.y - 1.0f, row.max.x, row.max.y);
        switch (g.style.tabShape) {
        case TabShape::Pill:
            // The segmented control's track, as wide as last frame's tabs (immediate mode: this frame's are not known
            // until they are submitted, and the track must be drawn under them).
            if (bar.usedWidthPrev > 0.0f) {
                w->dl->AddRectFilled(Rect(row.min.x, row.min.y, std::min(row.min.x + bar.usedWidthPrev, row.max.x), row.max.y),
                                     StyleColor(g, UiColor::FrameBg), g.style.tabRounding + 2.0f);
            }
            break;
        case TabShape::Underline: w->dl->AddRectFilled(line, StyleColor(g, UiColor::Separator)); break;
        case TabShape::Box: w->dl->AddRectFilled(line, StyleColor(g, UiColor::CheckMark)); break;
        default: w->dl->AddRectFilled(line, StyleColor(g, UiColor::TabActive)); break;
        }
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
    bar->usedWidthPrev = std::max(0.0f, bar->nextX - bar->startX - 2.0f);
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
        const float r = g.style.tabRounding;
        DrawList& dl = *w->dl;
        // Unselected labels are dimmed in the looks without a tab background.
        const Color dim = selected || hovered ? StyleColor(g, UiColor::Text) : StyleColor(g, UiColor::Text, 0.62f);
        switch (g.style.tabShape) {
        case TabShape::Underline: {
            if (hovered && !selected) dl.AddRectFilled(bb, StyleColor(g, UiColor::FrameBgHovered, 0.5f), CornerRadii(r, r, 0.0f, 0.0f));
            if (selected) {
                const Rect bar2(bb.min.x, bb.max.y - 2.0f, bb.max.x, bb.max.y);
                RenderGlow(g, bar2, 0.0f, 0.9f);
                dl.AddRectFilled(bar2, StyleColor(g, UiColor::CheckMark));
            }
            RenderTextAligned(g, bb, text, {0.5f, 0.5f}, nullptr, &textSize, dim);
            break;
        }
        case TabShape::Pill: {
            const Rect pill = bb.Expanded(-2.0f);
            if (selected) {
                if (g.style.knobShadow > 0.0f) {
                    dl.AddShadow(pill, StyleColor(g, UiColor::WindowShadow, g.style.knobShadow * 0.8f), 4.0f, r, {0.0f, 1.0f}, true);
                }
                RenderGlow(g, pill, r, 0.7f);
                RenderFrame(g, pill, StyleColor(g, UiColor::TabActive), false, r);
            } else if (hovered) {
                dl.AddRectFilled(pill, StyleColor(g, UiColor::TabHovered, 0.7f), r);
            }
            RenderTextAligned(g, bb, text, {0.5f, 0.5f}, nullptr, &textSize, dim);
            break;
        }
        case TabShape::Box: {
            const Rect box(bb.min, {bb.max.x, bb.max.y - 1.0f});
            RectStyle style;
            style.fill = selected ? StyleColor(g, UiColor::CheckMark) : StyleColor(g, hovered ? UiColor::TabHovered : UiColor::Tab);
            style.radii = CornerRadii(r, r, 0.0f, 0.0f);
            style.borderWidth = 1.0f;
            style.borderColor = StyleColor(g, selected ? UiColor::CheckMark : UiColor::Border);
            if (selected) RenderGlow(g, box, r, 0.7f);
            dl.AddRectEx(box, style);
            RenderTextAligned(g, bb, text, {0.5f, 0.5f}, nullptr, &textSize,
                              StyleColor(g, selected ? UiColor::AccentText : UiColor::Text));
            break;
        }
        default: {
            const UiColor color = selected ? UiColor::TabActive : hovered ? UiColor::TabHovered : UiColor::Tab;
            dl.AddRectFilled(Rect(bb.min, {bb.max.x, bb.max.y - (selected ? 0.0f : 1.0f)}), StyleColor(g, color),
                             CornerRadii(r, r, 0.0f, 0.0f));
            RenderTextAligned(g, bb, text, {0.5f, 0.5f}, nullptr, &textSize);
            break;
        }
        }
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
