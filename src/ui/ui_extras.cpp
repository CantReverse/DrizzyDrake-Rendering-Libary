// UI extras: toggle switches, spinners, text links, help markers, multi-component sliders and drags, angle sliders,
// text filters and notifications.
#include "ui_internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace drizzy {
using namespace ui_detail;

namespace {

Color LerpColor(Color a, Color b, float t) {
    auto channel = [&](int shift) {
        const float x = float((a >> shift) & 0xFFu), y = float((b >> shift) & 0xFFu);
        return uint32_t(x + (y - x) * t + 0.5f) << shift;
    };
    return channel(0) | channel(8) | channel(16) | channel(24);
}

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

// Case-insensitive (ASCII) search for `needle`, which is already lowercase.
bool ContainsLower(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    const size_t last = haystack.size() - needle.size();
    for (size_t i = 0; i <= last; ++i) {
        if (Lower(haystack[i]) != needle[0]) continue;
        size_t j = 1;
        while (j < needle.size() && Lower(haystack[i + j]) == needle[j]) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

Color NotifyColor(NotifyType type) {
    switch (type) {
    case NotifyType::Success: return Hex(0x4CC38A);
    case NotifyType::Warning: return Hex(0xF2B33D);
    case NotifyType::Error: return Hex(0xE5534B);
    default: return Hex(0x5B8CFF);
    }
}

// `count` widgets side by side in one item width, then the label: one group, so it acts as a single item.
template <typename Fn>
bool MultiComponent(Ui& ui, UiState& g, std::string_view label, int count, Fn component) {
    Window* w = g.current;
    if (w->skipItems || count <= 0) return false;
    const std::string_view text = VisibleText(label);
    const float total = ui.CalcItemWidth();
    const float spacing = g.style.itemInnerSpacing.x;
    const float each = std::max(1.0f, std::floor((total - spacing * float(count - 1)) / float(count)));
    bool changed = false;
    ui.BeginGroup();
    ui.PushID(label);
    for (int i = 0; i < count; ++i) {
        ui.SetNextItemWidth(i + 1 < count ? each : std::max(1.0f, total - (each + spacing) * float(count - 1)));
        ui.PushID(i);
        changed |= component(i);
        ui.PopID();
        if (i + 1 < count) ui.SameLine(0.0f, spacing);
    }
    ui.PopID();
    if (!text.empty()) {
        ui.SameLine(0.0f, spacing);
        ui.Text(text);
    }
    ui.EndGroup();
    return changed;
}

} // namespace

// =====================================================================================================================
// Toggle switch, spinner, link, help marker
// =====================================================================================================================
bool Ui::ToggleSwitch(std::string_view label, bool* value) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const float height = FrameHeight(g);
    const float trackH = std::floor(height * 0.8f);
    const float trackW = std::floor(trackH * 1.8f);
    const Vec2 pos = w->cursorPos;
    const float labelW = text.empty() ? 0.0f : g.style.itemInnerSpacing.x + TextSize(g, text).x;
    const Rect total(pos, {pos.x + trackW + labelW, pos.y + height});
    ItemSize(g, total.Size(), g.style.framePadding.y);
    if (!ItemAdd(g, total, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ButtonBehavior(g, total, id, &hovered, &held);
    if (pressed) {
        *value = !*value;
        w->lastItemStatus |= kItemEdited;
    }
    // The knob eases to its side; its position lives in the window's storage.
    const float target = *value ? 1.0f : 0.0f;
    float t = w->storage.GetFloat(id, target);
    t += (target - t) * std::min(1.0f, g.input.deltaTime * 14.0f);
    if (std::fabs(target - t) < 0.002f) t = target;
    w->storage.SetFloat(id, t);

    DrawList& dl = *w->dl;
    const Rect track = Rect::FromPosSize({pos.x, pos.y + std::floor((height - trackH) * 0.5f)}, {trackW, trackH});
    const Color off = StyleColor(g, hovered ? UiColor::FrameBgHovered : UiColor::FrameBg);
    dl.AddRectFilled(track, LerpColor(off, StyleColor(g, UiColor::CheckMark), t), trackH * 0.5f);
    const float radius = trackH * 0.5f - 2.0f;
    const float x = Lerp(track.min.x + 2.0f + radius, track.max.x - 2.0f - radius, t);
    dl.AddCircleFilled({x, track.Center().y}, radius, StyleColor(g, UiColor::Text));
    if (!text.empty()) {
        RenderText(g, {track.max.x + g.style.itemInnerSpacing.x, pos.y + g.style.framePadding.y}, text,
                   StyleColor(g, UiColor::Text));
    }
    return pressed;
}

void Ui::Spinner(std::string_view id, float radius, float thickness) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    (void)id;
    const float r = radius > 0.0f ? radius : g.fontSize * 0.5f;
    const float size = r * 2.0f + thickness;
    const Rect bb = Rect::FromPosSize(w->cursorPos, {size, size});
    ItemSize(g, bb.Size());
    if (!ItemAdd(g, bb, 0)) return;
    // A rotating arc whose length breathes, so motion reads even at a glance.
    const float time = float(g.time);
    const float start = std::fmod(time * 5.0f, 2.0f * kPi);
    const float sweep = kPi * (0.6f + 0.5f * (std::sin(time * 2.2f) + 1.0f));
    w->dl->AddArc(bb.Center(), r, start, start + sweep, StyleColor(g, UiColor::CheckMark), thickness);
}

bool Ui::TextLink(std::string_view label) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const Vec2 pos(w->cursorPos.x, w->cursorPos.y + w->currLineTextBaseOffset);
    const Vec2 size = TextSize(g, text);
    const Rect bb = Rect::FromPosSize(pos, size);
    ItemSize(g, size);
    if (!ItemAdd(g, bb, id)) return false;
    bool hovered = false, held = false;
    const bool pressed = ButtonBehavior(g, bb, id, &hovered, &held);
    if (hovered) g.cursorRequest = MouseCursor::Hand;
    const Color color = StyleColor(g, held ? UiColor::SliderGrabActive : UiColor::CheckMark);
    RenderText(g, pos, text, color);
    if (hovered) w->dl->AddRectFilled(Rect(bb.min.x, bb.max.y - 1.0f, bb.max.x, bb.max.y), color);
    return pressed;
}

void Ui::HelpMarker(std::string_view text) {
    TextDisabled("(?)");
    if (IsItemHovered()) SetTooltip(text);
}

// =====================================================================================================================
// Multi-component sliders and drags, angles
// =====================================================================================================================
bool Ui::SliderFloatN(std::string_view label, float* values, int count, float min, float max, const char* format) {
    return MultiComponent(*this, *m, label, count,
                          [&](int i) { return SliderFloat("##v", &values[i], min, max, format); });
}

bool Ui::SliderIntN(std::string_view label, int* values, int count, int min, int max, const char* format) {
    return MultiComponent(*this, *m, label, count,
                          [&](int i) { return SliderInt("##v", &values[i], min, max, format); });
}

bool Ui::DragFloatN(std::string_view label, float* values, int count, float speed, float min, float max,
                    const char* format) {
    return MultiComponent(*this, *m, label, count,
                          [&](int i) { return DragFloat("##v", &values[i], speed, min, max, format); });
}

bool Ui::DragIntN(std::string_view label, int* values, int count, float speed, int min, int max, const char* format) {
    return MultiComponent(*this, *m, label, count,
                          [&](int i) { return DragInt("##v", &values[i], speed, min, max, format); });
}

bool Ui::SliderAngle(std::string_view label, float* radians, float minDegrees, float maxDegrees, const char* format) {
    float degrees = *radians * (180.0f / kPi);
    const bool changed = SliderFloat(label, &degrees, minDegrees, maxDegrees, format);
    if (changed) *radians = degrees * (kPi / 180.0f);
    return changed;
}

// =====================================================================================================================
// TextFilter
// =====================================================================================================================
bool TextFilter::Draw(Ui& ui, std::string_view label, float width) {
    if (width != 0.0f) ui.SetNextItemWidth(width);
    const bool changed = ui.InputTextWithHint(label, "include,-exclude", m_text, sizeof(m_text));
    if (changed) Parse();
    return changed;
}

void TextFilter::Clear() { Set({}); }

void TextFilter::Set(std::string_view text) {
    const size_t n = std::min(text.size(), sizeof(m_text) - 1);
    std::memcpy(m_text, text.data(), n);
    m_text[n] = '\0';
    Parse();
}

void TextFilter::Parse() {
    m_termCount = 0;
    m_hasInclude = false;
    size_t length = 0;
    for (; m_text[length] != '\0' && length + 1 < sizeof(m_lower); ++length) m_lower[length] = Lower(m_text[length]);
    m_lower[length] = '\0';
    size_t i = 0;
    while (i < length && m_termCount < kMaxTerms) {
        size_t start = i;
        while (i < length && m_lower[i] != ',') ++i;
        size_t end = i++;
        while (start < end && m_lower[start] == ' ') ++start;
        while (end > start && m_lower[end - 1] == ' ') --end;
        Term term;
        term.exclude = start < end && m_lower[start] == '-';
        if (term.exclude) ++start;
        if (start >= end) continue;
        term.offset = uint16_t(start);
        term.length = uint16_t(end - start);
        m_terms[m_termCount++] = term;
        m_hasInclude |= !term.exclude;
    }
}

bool TextFilter::PassFilter(std::string_view text) const {
    if (m_termCount == 0) return true;
    bool included = !m_hasInclude;
    for (int i = 0; i < m_termCount; ++i) {
        const Term& term = m_terms[i];
        const bool found = ContainsLower(text, std::string_view(m_lower + term.offset, term.length));
        if (term.exclude && found) return false;
        if (!term.exclude && found) included = true;
    }
    return included;
}

// =====================================================================================================================
// Notifications
// =====================================================================================================================
void Ui::Notify(std::string_view text, NotifyType type, float seconds) {
    UiState& g = *m;
    if (g.notifications.size() >= 16) g.notifications.erase(g.notifications.begin());  // oldest goes first
    Notification n;
    n.text.assign(text.data(), text.size());
    n.type = type;
    n.duration = std::max(seconds, 0.5f);
    g.notifications.push_back(std::move(n));
}

namespace ui_detail {

void RenderNotifications(UiState& g) {
    if (g.notifications.empty() || !g.font) return;
    constexpr float kFadeIn = 0.15f, kFadeOut = 0.4f;
    constexpr int kMaxVisible = 5;
    const float margin = 16.0f;
    const Vec2 pad = g.style.windowPadding;
    const float width = std::min(340.0f, g.displaySize.x - margin * 2.0f);
    if (width <= pad.x * 2.0f + 8.0f) return;
    const float textWidth = width - pad.x * 2.0f - 6.0f;
    float bottom = g.displaySize.y - margin;
    bool anyHovered = false;
    int shown = 0;
    // Newest at the bottom; older ones stack above it.
    for (size_t k = g.notifications.size(); k-- > 0;) {
        Notification& n = g.notifications[k];
        if (shown == kMaxVisible) {
            n.age += g.input.deltaTime;  // hidden ones still expire
            continue;
        }
        const Vec2 textSize = TextSize(g, n.text, textWidth);
        const float height = textSize.y + pad.y * 2.0f;
        const Rect r(g.displaySize.x - margin - width, bottom - height, g.displaySize.x - margin, bottom);
        const bool hovered = r.Contains(g.mousePos);
        anyHovered |= hovered;
        if (!hovered) n.age += g.input.deltaTime;  // hovering keeps it up
        const float alpha = Clamp(std::min(n.age / kFadeIn, (n.duration - n.age) / kFadeOut), 0.0f, 1.0f);
        if (alpha > 0.0f) {
            const float rounding = g.style.popupRounding;
            g.foreground.AddShadow(r, ScaleAlpha(StyleColor(g, UiColor::WindowShadow), alpha * 0.6f), 10.0f, rounding,
                                   {0.0f, 3.0f}, true);
            g.foreground.AddRectFilled(r, ScaleAlpha(StyleColor(g, UiColor::PopupBg), alpha), rounding);
            g.foreground.AddRectFilled(Rect(r.min.x, r.min.y, r.min.x + 4.0f, r.max.y),
                                       ScaleAlpha(NotifyColor(n.type), alpha), CornerRadii(rounding, 0.0f, 0.0f, rounding));
            TextStyle style;
            style.color = ScaleAlpha(StyleColor(g, UiColor::Text), alpha);
            style.wrapWidth = textWidth;
            g.foreground.AddText(*g.font, g.fontSize, {r.min.x + pad.x + 6.0f, r.min.y + pad.y}, n.text, style);
        }
        bottom -= height + 8.0f;
        ++shown;
    }
    g.notifications.erase(std::remove_if(g.notifications.begin(), g.notifications.end(),
                                         [](const Notification& n) { return n.age >= n.duration; }),
                          g.notifications.end());
    if (anyHovered) g.output.wantCaptureMouse = true;
}

} // namespace ui_detail
} // namespace drizzy
