// UI core: frame lifecycle, input, windows, layout, IDs, popups, style stacks and queries.
#include "ui_internal.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace drizzy {
using namespace ui_detail;

namespace {

constexpr float kDoubleClickTime = 0.30f;
constexpr float kDoubleClickMaxDist = 6.0f;
constexpr float kKeyRepeatDelay = 0.275f;
constexpr float kKeyRepeatRate = 0.050f;
constexpr uint32_t kCondAllBits = 0xFFu;
constexpr float kResizeBorderHalfWidth = 4.0f;  // window edges can be grabbed this far inside and outside the border

enum ResizeEdge : uint32_t { kEdgeLeft = 1u << 0, kEdgeRight = 1u << 1, kEdgeTop = 1u << 2, kEdgeBottom = 1u << 3 };

uint32_t CondBit(Cond cond) { return 1u << uint32_t(cond); }

// The edges of window rect `r` that a press at `p` grabs: one along an edge, both near a corner (within `cornerSize`
// of it, so corners are easy to hit), none away from the border.
uint32_t ResizeEdgesAt(const Rect& r, Vec2 p, float cornerSize) {
    const float half = kResizeBorderHalfWidth;
    if (!r.Expanded(half).Contains(p)) return 0;
    uint32_t edges = 0;
    if (p.x < r.min.x + half) edges |= kEdgeLeft;
    else if (p.x >= r.max.x - half) edges |= kEdgeRight;
    if (p.y < r.min.y + half) edges |= kEdgeTop;
    else if (p.y >= r.max.y - half) edges |= kEdgeBottom;
    if (edges & (kEdgeLeft | kEdgeRight)) {
        if (p.y < r.min.y + cornerSize) edges |= kEdgeTop;
        else if (p.y >= r.max.y - cornerSize) edges |= kEdgeBottom;
    }
    if (edges & (kEdgeTop | kEdgeBottom)) {
        if (p.x < r.min.x + cornerSize) edges |= kEdgeLeft;
        else if (p.x >= r.max.x - cornerSize) edges |= kEdgeRight;
    }
    return edges;
}

MouseCursor ResizeCursor(uint32_t edges) {
    const bool horizontal = (edges & (kEdgeLeft | kEdgeRight)) != 0;
    const bool vertical = (edges & (kEdgeTop | kEdgeBottom)) != 0;
    if (horizontal && vertical) {
        const bool nwse = ((edges & kEdgeLeft) != 0) == ((edges & kEdgeTop) != 0);
        return nwse ? MouseCursor::ResizeNWSE : MouseCursor::ResizeNESW;
    }
    return horizontal ? MouseCursor::ResizeEW : MouseCursor::ResizeNS;
}

// Whether a SetNext...() with `cond` may apply now; consumes one-shot conditions.
bool CondAllowed(uint32_t& allowed, Cond cond) {
    if (cond == Cond::Always) return true;
    if (!(allowed & CondBit(cond))) return false;
    if (cond == Cond::Once) allowed &= ~CondBit(Cond::Once);
    return true;
}

void UpdateAlpha(UiState& g) { g.alpha = g.style.alpha * (g.disabled > 0 ? g.style.disabledAlpha : 1.0f); }

UiState::Window* CreateWindowImpl(UiState& g, std::string_view name, ID id, uint32_t flags) {
    auto window = std::make_unique<Window>();
    Window* w = window.get();
    w->id = id;
    w->name.assign(name);
    w->flags = flags;
    w->root = w;
    w->setPosAllowed = w->setSizeAllowed = w->setCollapsedAllowed = kCondAllBits;
    if (!(flags & (kWindowChild | kWindowPopup | kWindowTooltip))) {
        // Cascade new top-level windows so they do not stack exactly on top of each other.
        const float offset = 24.0f * float(g.rootWindowsCreated % 10);
        w->pos = {60.0f + offset, 60.0f + offset};
        g.rootWindowsCreated++;
        g.focusOrder.push_back(w);
    }
    w->drawList.SetPrimAllocator(g.primAllocator);
    g.windowsById[id] = w;
    g.windows.push_back(std::move(window));
    return w;
}

Vec2 CalcAutoFitSize(const UiState& g, const Window* w, float maxHeight) {
    Vec2 size = w->contentSize + w->padding * 2.0f;
    size.x += w->borderSize * 2.0f;
    size.y += std::max(w->titleBarHeight, w->borderSize) + w->borderSize + w->menuBarHeight;
    if (w->titleBarHeight > 0.0f) {
        // Room for the title and its buttons.
        const float buttons = (w->flags & WindowFlags::NoCollapse ? 0.0f : g.fontSize + g.style.itemInnerSpacing.x) +
                              g.fontSize + g.style.itemInnerSpacing.x;
        const auto nm = w->name.decode();
        size.x = std::max(size.x, TextSize(g, VisibleText(nm.view())).x + g.style.framePadding.x * 2.0f + buttons);
    }
    if (maxHeight > 0.0f && size.y > maxHeight) {
        size.y = maxHeight;
        size.x += g.style.scrollbarSize;
    }
    if (!(w->flags & kWindowChild) && g.displaySize.x > 0.0f) {
        size = MinV(size, g.displaySize - g.style.windowPadding * 2.0f);
    }
    return {std::floor(size.x), std::floor(size.y)};
}

void UpdateHoveredWindow(UiState& g) {
    g.hoveredWindow = g.hoveredWindowInner = nullptr;
    const Vec2 mouse = g.mousePos;
    if (!IsMouseValid(mouse)) return;
    if (g.movingWindow) {
        g.hoveredWindow = g.hoveredWindowInner = g.movingWindow;
        return;
    }
    // Nothing below the top-most modal can be reached.
    size_t modalLevel = SIZE_MAX;
    for (size_t i = g.openPopups.size(); i-- > 0;) {
        if (g.openPopups[i].modal) {
            modalLevel = i;
            break;
        }
    }
    Window* hovered = nullptr;
    for (size_t i = g.openPopups.size(); i-- > 0 && !hovered;) {
        Window* w = g.openPopups[i].window;
        if (w && w->active && !w->hidden && w->outerRect.Contains(mouse)) hovered = w;
        if (i == modalLevel) break;
    }
    if (!hovered && modalLevel == SIZE_MAX) {
        for (size_t i = g.focusOrder.size(); i-- > 0;) {
            Window* w = g.focusOrder[i];
            // Resizable windows also take the mouse just outside their border, where their edges can be grabbed.
            const Rect hitRect = w->resizable ? w->outerRect.Expanded(kResizeBorderHalfWidth) : w->outerRect;
            if (w->active && !w->hidden && !(w->flags & WindowFlags::NoInputs) && hitRect.Contains(mouse)) {
                hovered = w;
                break;
            }
        }
    }
    g.hoveredWindow = g.hoveredWindowInner = hovered;
    if (!hovered) return;
    // The innermost child under the mouse (for wheel scrolling) is the one begun last.
    int bestOrder = -1;
    for (const auto& w : g.windows) {
        if (w->active && w->root == hovered && w.get() != hovered && w->beginOrder > bestOrder &&
            w->clipRect.Contains(mouse)) {
            g.hoveredWindowInner = w.get();
            bestOrder = w->beginOrder;
        }
    }
}

// Closes the popups stacked above the one containing `ref` (all of them when `ref` is not in a popup), never past a
// modal.
void ClosePopupsOverWindow(UiState& g, Window* ref) {
    if (g.openPopups.empty()) return;
    size_t keep = 0;
    if (ref) {
        for (size_t i = 0; i < g.openPopups.size(); ++i) {
            if (g.openPopups[i].window == ref->root) {
                keep = i + 1;
                break;
            }
        }
    }
    for (size_t i = g.openPopups.size(); i-- > keep;) {
        if (g.openPopups[i].modal) {
            keep = i + 1;
            break;
        }
    }
    if (keep < g.openPopups.size()) {
        g.popupClosedByClick = g.openPopups[keep].popupId;
        ClosePopupToLevel(g, keep);
    }
}

} // namespace

// =====================================================================================================================
// Shared helpers
// =====================================================================================================================
namespace ui_detail {

// Eight bytes per step: multiply-xorshift mixing of 64-bit words, folded to 32 bits. Labels are short, so this is a
// handful of multiplies where a byte-wise hash would chain one per character.
ID HashData(const void* data, size_t size, ID seed) {
    constexpr uint64_t kMul = 0x9E3779B97F4A7C15ull;
    const auto* p = static_cast<const uint8_t*>(data);
    uint64_t h = ((uint64_t(seed) << 32) | seed) ^ (uint64_t(size) * 0xC2B2AE3D27D4EB4Full);
    for (; size >= 8; size -= 8, p += 8) {
        uint64_t word;
        std::memcpy(&word, p, 8);
        h = (h ^ word) * kMul;
        h ^= h >> 29;
    }
    if (size > 0) {
        uint64_t word = 0;
        for (size_t i = 0; i < size; ++i) word |= uint64_t(p[i]) << (8 * i);
        h = (h ^ word) * kMul;
        h ^= h >> 29;
    }
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    const ID id = ID(h);
    return id ? id : 1u;
}

Label ParseLabel(std::string_view label, ID seed) {
    // One pass over the '#' characters (most labels have none): the first "##" ends the visible text, the first
    // "###" starts the part the ID is hashed from.
    std::string_view text = label, hashed = label;
    bool textCut = false;
    const char* begin = label.data();
    const char* end = begin + label.size();
    for (const char* p = begin; p < end;) {
        const char* mark = static_cast<const char*>(std::memchr(p, '#', size_t(end - p)));
        if (!mark || mark + 1 >= end) break;
        if (mark[1] != '#') {
            p = mark + 1;
            continue;
        }
        if (!textCut) {
            text = label.substr(0, size_t(mark - begin));
            textCut = true;
        }
        if (mark + 2 < end && mark[2] == '#') {
            hashed = label.substr(size_t(mark - begin));
            break;
        }
        p = mark + 2;
    }
    return {HashData(hashed.data(), hashed.size(), seed), text};
}

ID HashStr(std::string_view str, ID seed) { return ParseLabel(str, seed).id; }

std::string_view VisibleText(std::string_view label) {
    const size_t hidden = label.find("##");
    return hidden == std::string_view::npos ? label : label.substr(0, hidden);
}

bool Storage::Has(ID key) const {
    const auto it = std::lower_bound(m_pairs.begin(), m_pairs.end(), key,
                                     [](const Pair& p, ID k) { return p.key < k; });
    return it != m_pairs.end() && it->key == key;
}

int Storage::GetInt(ID key, int defaultValue) const {
    const auto it = std::lower_bound(m_pairs.begin(), m_pairs.end(), key,
                                     [](const Pair& p, ID k) { return p.key < k; });
    return (it != m_pairs.end() && it->key == key) ? it->value : defaultValue;
}

void Storage::SetInt(ID key, int value) {
    const auto it = std::lower_bound(m_pairs.begin(), m_pairs.end(), key,
                                     [](const Pair& p, ID k) { return p.key < k; });
    if (it != m_pairs.end() && it->key == key) {
        it->value = value;
    } else {
        m_pairs.insert(it, {key, value});
    }
}

float Storage::GetFloat(ID key, float defaultValue) const {
    if (!Has(key)) return defaultValue;
    const int bits = GetInt(key);
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void Storage::SetFloat(ID key, float value) {
    int bits;
    std::memcpy(&bits, &value, sizeof(bits));
    SetInt(key, bits);
}

bool IsMouseValid(Vec2 pos) { return pos.x > -1e29f && pos.y > -1e29f; }

ID MoveId(const Window* window) { return HashStr(DZ_STR("#MOVE"), window->id); }

UiState::Window* FindWindow(UiState& g, ID id) {
    const auto it = g.windowsById.find(id);
    return it == g.windowsById.end() ? nullptr : it->second;
}

void FocusWindow(UiState& g, Window* window) {
    g.focusedWindow = window ? window->root : nullptr;
    if (!window) return;
    Window* root = window->root;
    if (root->flags & (kWindowPopup | kWindowTooltip | WindowFlags::NoBringToFrontOnFocus)) return;
    const auto it = std::find(g.focusOrder.begin(), g.focusOrder.end(), root);
    if (it != g.focusOrder.end() && it + 1 != g.focusOrder.end()) {
        g.focusOrder.erase(it);
        g.focusOrder.push_back(root);
    }
}

void SetActiveID(UiState& g, ID id, Window* window) {
    g.activeIdJustActivated = id != 0 && g.activeId != id;
    g.activeId = id;
    g.activeIdWindow = window;
    g.activeIdAlive = id != 0;
}

void ClearActiveID(UiState& g) { SetActiveID(g, 0, nullptr); }

void KeepAliveID(UiState& g, ID id) {
    if (g.activeId == id) g.activeIdAlive = true;
}

bool IsPopupOpenId(const UiState& g, ID id) {
    const size_t level = g.beginPopupStack.size();
    return g.openPopups.size() > level && g.openPopups[level].popupId == id;
}

void OpenPopupEx(UiState& g, ID id) {
    const size_t level = g.beginPopupStack.size();
    if (g.openPopups.size() > level && g.openPopups[level].popupId == id) {
        g.openPopups.resize(level + 1);  // already open: just close anything stacked on top of it
        return;
    }
    PopupData popup;
    popup.popupId = id;
    popup.openFrame = g.frameCount;
    popup.openMousePos = g.mousePos;
    g.openPopups.resize(std::min(g.openPopups.size(), level));
    g.openPopups.push_back(popup);
}

bool BeginPopupEx(Ui& ui, UiState& g, ID id, uint32_t flags) {
    if (!IsPopupOpenId(g, id)) {
        g.nextWindow.Clear();
        return false;
    }
    const size_t level = g.beginPopupStack.size();
    if (!g.nextWindow.hasPos) {
        g.nextWindow.hasPos = true;
        g.nextWindow.pos = g.openPopups[level].openMousePos;
        g.nextWindow.posCond = Cond::Appearing;
        g.nextWindow.pivot = {};
    }
    flags |= kWindowPopup;
    if (!(flags & kWindowModal)) flags |= WindowFlags::NoTitleBar | WindowFlags::NoMove | WindowFlags::NoResize;
    if (!g.nextWindow.hasSize) flags |= WindowFlags::AlwaysAutoResize;
    char name[32];
    std::snprintf(name, sizeof(name), DZ_STR("##Popup_%08X"), id);
    g.beginPopupStack.push_back(g.openPopups[level]);
    const bool visible = ui.Begin(name, nullptr, flags);
    if (level < g.openPopups.size()) g.openPopups[level].window = g.current;
    if (!visible) {
        ui.EndPopup();
        return false;
    }
    return true;
}

void ClosePopupToLevel(UiState& g, size_t level) {
    if (level >= g.openPopups.size()) return;
    // Hand focus back to whatever was below the closed popups.
    if (g.focusedWindow && (g.focusedWindow->flags & kWindowPopup)) {
        g.focusedWindow = level > 0 ? g.openPopups[level - 1].window : nullptr;
        if (!g.focusedWindow && !g.focusOrder.empty()) g.focusedWindow = g.focusOrder.back();
    }
    g.openPopups.resize(level);
}

// ---- Layout ---------------------------------------------------------------------------------------------------------
float TextLineHeight(const UiState& g) { return std::floor(g.font->LineHeight() * g.fontSize + 0.5f); }

float FrameHeight(const UiState& g) { return TextLineHeight(g) + g.style.framePadding.y * 2.0f; }

Vec2 TextSize(const UiState& g, std::string_view text, float wrapWidth) {
    if (text.empty()) return {};
    const Vec2 size = g.font->MeasureText(text, g.fontSize, wrapWidth);
    const float lines = std::floor(size.y / (g.font->LineHeight() * g.fontSize) + 0.5f);
    return {std::ceil(size.x), lines * TextLineHeight(g)};
}

void ItemSize(UiState& g, Vec2 size, float textBaseOffset) {
    Window* w = g.current;
    if (w->skipItems) return;
    const float offsetToMatchBaseline = std::max(0.0f, w->currLineTextBaseOffset - textBaseOffset);
    const float lineY = w->cursorPos.y;
    const float lineHeight = std::max(w->currLineHeight, size.y + offsetToMatchBaseline);
    w->cursorPosPrevLine = {w->cursorPos.x + size.x, lineY};
    w->cursorPos.x = std::floor(w->contentStartX + w->indent);
    w->cursorPos.y = std::floor(lineY + lineHeight + g.style.itemSpacing.y);
    w->cursorMaxPos.x = std::max(w->cursorMaxPos.x, w->cursorPosPrevLine.x);
    w->cursorMaxPos.y = std::max(w->cursorMaxPos.y, w->cursorPos.y - g.style.itemSpacing.y);
    w->prevLineHeight = lineHeight;
    w->currLineHeight = 0.0f;
    w->prevLineTextBaseOffset = std::max(w->currLineTextBaseOffset, textBaseOffset);
    w->currLineTextBaseOffset = 0.0f;
}

bool IsMouseHovering(const UiState& g, const Rect& r) {
    return r.Intersect(g.current->clipRect).Contains(g.mousePos);
}

bool ItemAdd(UiState& g, const Rect& bb, ID id) {
    Window* w = g.current;
    w->lastItemId = id;
    w->lastItemRect = bb;
    w->lastItemStatus = 0;
    g.nextItem.hasWidth = false;
    if (id) KeepAliveID(g, id);
    if (!bb.Overlaps(w->clipRect)) return false;
    w->lastItemStatus |= kItemVisible;
    if (IsMouseHovering(g, bb)) w->lastItemStatus |= kItemHoveredRect;
    return true;
}

bool ItemHoverable(UiState& g, const Rect& bb, ID id) {
    const Window* w = g.current;
    if (g.hoveredWindow != w->root || g.disabled > 0) return false;
    if (g.activeId != 0 && g.activeId != id) return false;
    if (g.hoveredId != 0 && g.hoveredId != id) return false;  // an earlier overlapping item has the mouse
    if (!IsMouseHovering(g, bb)) return false;
    if (id) g.hoveredId = id;
    return true;
}

bool ButtonBehavior(UiState& g, const Rect& bb, ID id, bool* outHovered, bool* outHeld, uint32_t flags) {
    const int button = (flags & kButtonMouseRight) ? 1 : 0;
    const bool hovered = ItemHoverable(g, bb, id);
    bool pressed = false;
    if (hovered && g.mouseClicked[button]) {
        if (flags & kButtonPressOnClick) {
            pressed = true;
        } else {
            SetActiveID(g, id, g.current);
            g.activeIdClickOffset = g.mousePos - bb.min;
        }
    }
    bool held = false;
    if (g.activeId == id) {
        // Window decorations (resize grip, scrollbar, title bar buttons) never pass through ItemAdd, so keep the ID
        // alive here too: otherwise NewFrame drops it a frame after the press and the drag stops.
        KeepAliveID(g, id);
        if (g.input.mouseDown[button]) {
            held = true;
        } else {
            if (hovered) pressed = true;  // released over the item
            ClearActiveID(g);
        }
    }
    if (outHovered) *outHovered = hovered;
    if (outHeld) *outHeld = held;
    return pressed;
}

float CalcItemWidth(const UiState& g) {
    const Window* w = g.current;
    float width = g.nextItem.hasWidth ? g.nextItem.width : w->itemWidth;
    if (width < 0.0f) width = std::max(1.0f, w->contentRegionMax.x - w->cursorPos.x + width);
    return std::floor(width);
}

Vec2 CalcItemSize(const UiState& g, Vec2 size, float defaultW, float defaultH) {
    const Window* w = g.current;
    const Vec2 avail = w->contentRegionMax - w->cursorPos;
    const float x = size.x == 0.0f ? defaultW : (size.x < 0.0f ? std::max(4.0f, avail.x + size.x) : size.x);
    const float y = size.y == 0.0f ? defaultH : (size.y < 0.0f ? std::max(4.0f, avail.y + size.y) : size.y);
    return {std::floor(x), std::floor(y)};
}

bool IsKeyPressedImpl(const UiState& g, Key key, bool repeat) {
    const float t = g.keysDownDuration[size_t(key)];
    if (t == 0.0f) return true;
    if (!repeat || t < kKeyRepeatDelay) return false;
    const float t0 = g.keysDownDurationPrev[size_t(key)];
    const int count0 = t0 < kKeyRepeatDelay ? -1 : int((t0 - kKeyRepeatDelay) / kKeyRepeatRate);
    const int count1 = int((t - kKeyRepeatDelay) / kKeyRepeatRate);
    return count1 > count0;
}

// ---- Rendering ------------------------------------------------------------------------------------------------------
Color StyleColor(const UiState& g, UiColor color, float alphaScale) {
    return ScaleAlpha(g.style.colors[size_t(color)], g.alpha * alphaScale);
}

// Fill (or, with UiStyle::gradient, a vertical light-to-dark shading of it) for a RectStyle.
static void SetShadedFill(const UiState& g, RectStyle& style, Color fill) {
    style.fill = fill;
    if (g.style.gradient <= 0.0f) return;
    const float k = Clamp(g.style.gradient, 0.0f, 1.0f) * 0.4f;
    const uint32_t a = ColorAlpha(fill);
    style.fill = LerpColor(fill, WithAlpha(colors::White, a), k);
    style.fillEnd = LerpColor(fill, WithAlpha(colors::Black, a), k);
    style.gradient = Gradient::Vertical;
}

// UiStyle::bevel: a light line along the straight part of the top edge, inside `inset` pixels of border.
static void RenderBevel(UiState& g, const Rect& r, float rounding, float inset) {
    if (g.style.bevel <= 0.0f) return;
    const float x = std::max(rounding * 0.7f, 1.0f) + inset;
    if (r.Width() <= x * 2.0f) return;
    g.current->dl->AddRectFilled(Rect(r.min.x + x, r.min.y + inset, r.max.x - x, r.min.y + inset + 1.0f),
                                 ScaleAlpha(colors::White, Clamp(g.style.bevel, 0.0f, 1.0f) * 0.45f * g.alpha));
}

void RenderFrame(UiState& g, const Rect& r, Color fill, bool border, float rounding) {
    DrawList& dl = *g.current->dl;
    const bool withBorder = border && g.style.frameBorderSize > 0.0f;
    if (withBorder || g.style.gradient > 0.0f) {
        RectStyle style;
        SetShadedFill(g, style, fill);
        style.radii = rounding;
        if (withBorder) {
            style.borderWidth = g.style.frameBorderSize;
            style.borderColor = StyleColor(g, UiColor::Border);
        }
        dl.AddRectEx(r, style);  // fill and border in one prim
    } else {
        dl.AddRectFilled(r, fill, rounding);
    }
    RenderBevel(g, r, rounding, withBorder ? g.style.frameBorderSize : 0.0f);
}

void RenderGlow(UiState& g, const Rect& r, float rounding, float strength) {
    const float size = g.style.glowSize;
    if (size <= 0.0f || strength <= 0.0f) return;
    g.current->dl->AddShadow(r, StyleColor(g, UiColor::Glow, strength), size, rounding, {}, true);
}

void RenderFieldFrame(UiState& g, const Rect& r, bool hovered, bool active) {
    const UiStyle& s = g.style;
    DrawList& dl = *g.current->dl;
    const UiColor bg = active ? UiColor::FrameBgActive : hovered ? UiColor::FrameBgHovered : UiColor::FrameBg;
    switch (s.frameShape) {
    case FrameShape::Outline: {
        if (active) RenderGlow(g, r, s.frameRounding, 0.6f);
        RectStyle style;
        SetShadedFill(g, style, StyleColor(g, bg, 0.6f));
        style.radii = s.frameRounding;
        style.borderWidth = std::max(s.frameBorderSize, 1.0f);
        const Color border = StyleColor(g, UiColor::Border), accent = StyleColor(g, UiColor::CheckMark);
        style.borderColor = active ? accent : hovered ? LerpColor(border, accent, 0.6f) : border;
        dl.AddRectEx(r, style);
        break;
    }
    case FrameShape::Underline: {
        if (hovered || active) {
            dl.AddRectFilled(r, StyleColor(g, UiColor::FrameBgHovered, active ? 0.55f : 0.35f),
                             CornerRadii(s.frameRounding, s.frameRounding, 0.0f, 0.0f));
        }
        const float thickness = active ? 2.0f : 1.0f;
        const Rect line(r.min.x, r.max.y - thickness, r.max.x, r.max.y);
        if (active) RenderGlow(g, line, 0.0f, 0.7f);
        dl.AddRectFilled(line, active    ? StyleColor(g, UiColor::CheckMark)
                               : hovered ? StyleColor(g, UiColor::Text, 0.7f)
                                         : StyleColor(g, UiColor::TextDisabled, 0.8f));
        break;
    }
    default:
        RenderFrame(g, r, StyleColor(g, bg), true, s.frameRounding);
        break;
    }
}

Rect RenderButtonFrame(UiState& g, const Rect& r, bool hovered, bool held, float rounding) {
    const UiStyle& s = g.style;
    DrawList& dl = *g.current->dl;
    const bool pressed = held && hovered;
    Rect bb = r;
    if (s.hardShadow.x != 0.0f || s.hardShadow.y != 0.0f) {
        if (pressed) bb = r.Translated(s.hardShadow);  // pushed down into its shadow
        else dl.AddRectFilled(r.Translated(s.hardShadow), StyleColor(g, UiColor::WindowShadow), rounding);
    }
    if (s.buttonShape == ButtonShape::Outline) {
        if (hovered) RenderGlow(g, bb, rounding, pressed ? 1.0f : 0.5f);
        RectStyle style;
        SetShadedFill(g, style, StyleColor(g, pressed ? UiColor::HeaderActive : hovered ? UiColor::HeaderHovered
                                                                                         : UiColor::Header, 0.5f));
        style.radii = rounding;
        style.borderWidth = std::max(s.frameBorderSize, 1.0f);
        const Color accent = StyleColor(g, UiColor::CheckMark);
        style.borderColor = hovered ? accent : LerpColor(StyleColor(g, UiColor::Border), accent, 0.45f);
        dl.AddRectEx(bb, style);
        RenderBevel(g, bb, rounding, style.borderWidth);
    } else {
        if (hovered) RenderGlow(g, bb, rounding, pressed ? 0.8f : 0.4f);
        const UiColor color = pressed ? UiColor::ButtonActive : hovered ? UiColor::ButtonHovered : UiColor::Button;
        RenderFrame(g, bb, StyleColor(g, color), true, rounding);
    }
    return bb;
}

void RenderKnob(UiState& g, Vec2 center, float radius, Color color, float glow) {
    DrawList& dl = *g.current->dl;
    const Rect box = Rect::FromCenter(center, {radius, radius});
    if (g.style.knobShadow > 0.0f) {
        dl.AddShadow(box, StyleColor(g, UiColor::WindowShadow, g.style.knobShadow), std::max(3.0f, radius * 0.45f),
                     radius, {0.0f, std::max(1.0f, radius * 0.15f)}, true);
    }
    RenderGlow(g, box, radius, glow);
    dl.AddCircleFilled(center, radius, color);
}

Color KnobColor(const UiState& g, Color flat) {
    if (g.style.knobShadow <= 0.0f) return flat;
    return ScaleAlpha(LerpColor(colors::White, g.style.colors[size_t(UiColor::Text)], 0.06f), g.alpha);
}

void RenderCheckBox(UiState& g, const Rect& r, bool hovered, bool held, bool round) {
    const UiStyle& s = g.style;
    DrawList& dl = *g.current->dl;
    const UiColor bg = held ? UiColor::FrameBgActive : hovered ? UiColor::FrameBgHovered : UiColor::FrameBg;
    const float radius = (r.Width() - 1.0f) * 0.5f;
    if (s.frameShape == FrameShape::Filled) {
        if (round) dl.AddCircleFilled(r.Center(), radius, StyleColor(g, bg));
        else RenderFrame(g, r, StyleColor(g, bg), true, s.frameRounding);
        return;
    }
    // Outlined: a faint fill inside a ring that takes the accent color under the mouse.
    const float width = std::max(s.frameBorderSize, s.frameShape == FrameShape::Underline ? 1.5f : 1.0f);
    const Color fill = StyleColor(g, bg, s.frameShape == FrameShape::Underline ? 0.0f : 0.6f);
    const Color border = hovered ? StyleColor(g, UiColor::CheckMark) : StyleColor(g, UiColor::TextDisabled, 0.9f);
    if (round) {
        if (ColorAlpha(fill)) dl.AddCircleFilled(r.Center(), radius, fill);
        dl.AddCircle(r.Center(), radius, border, width);
    } else {
        RectStyle style;
        style.fill = fill;
        style.radii = std::min(s.frameRounding, r.Width() * 0.5f);
        style.borderWidth = width;
        style.borderColor = border;
        dl.AddRectEx(r, style);
    }
}

void RenderText(UiState& g, Vec2 pos, std::string_view text, Color color, float wrapWidth) {
    if (text.empty() || ColorAlpha(color) == 0) return;
    if (wrapWidth > 0.0f) {
        TextStyle style;
        style.color = color;
        style.wrapWidth = wrapWidth;
        g.current->dl->AddText(*g.font, g.fontSize, pos, text, style);
    } else {
        g.current->dl->AddText(*g.font, g.fontSize, pos, color, text);
    }
}

void RenderTextClipped(UiState& g, Vec2 pos, std::string_view text, Color color, const Rect& clip) {
    if (text.empty() || ColorAlpha(color) == 0) return;
    g.current->dl->AddTextClipped(*g.font, g.fontSize, pos, color, text, clip);
}

void RenderTextAligned(UiState& g, const Rect& r, std::string_view text, Vec2 align, const Rect* clip,
                       const Vec2* knownSize) {
    RenderTextAligned(g, r, text, align, clip, knownSize, StyleColor(g, UiColor::Text));
}

void RenderTextAligned(UiState& g, const Rect& r, std::string_view text, Vec2 align, const Rect* clip,
                       const Vec2* knownSize, Color color) {
    if (text.empty()) return;
    const Vec2 size = knownSize ? *knownSize : TextSize(g, text);
    Vec2 pos = r.min + MaxV(Vec2(), (r.Size() - size) * align);
    pos = {std::floor(pos.x), std::floor(pos.y)};
    // Text that overflows is trimmed glyph by glyph, which keeps it in the current draw command.
    const Rect clipRect = clip ? *clip : r;
    const bool needClip = pos.x + size.x > clipRect.max.x || pos.x < clipRect.min.x;
    if (needClip) {
        RenderTextClipped(g, pos, text, color, clipRect);
    } else {
        RenderText(g, pos, text, color);
    }
}

void RenderTextOverFill(UiState& g, const Rect& r, std::string_view text, float fillX) {
    if (text.empty()) return;
    const Vec2 size = TextSize(g, text);
    const Vec2 pos(std::floor(r.min.x + std::max(0.0f, (r.Width() - size.x) * 0.5f)),
                   std::floor(r.min.y + std::max(0.0f, (r.Height() - size.y) * 0.5f)));
    const Color over = StyleColor(g, UiColor::AccentText), past = StyleColor(g, UiColor::Text);
    if (fillX <= pos.x) {
        RenderTextAligned(g, r, text, {0.5f, 0.5f}, nullptr, &size, past);
    } else if (fillX >= pos.x + size.x) {
        RenderTextAligned(g, r, text, {0.5f, 0.5f}, nullptr, &size, over);
    } else {
        // Split where the fill ends: each half is trimmed glyph by glyph, so no clip rect (or draw call) is added.
        RenderTextClipped(g, pos, text, over, Rect(r.min.x, r.min.y, fillX, r.max.y));
        RenderTextClipped(g, pos, text, past, Rect(fillX, r.min.y, r.max.x, r.max.y));
    }
}

void RenderArrow(UiState& g, Vec2 c, float size, int dir, Color color) {
    const float r = size * 0.5f;
    DrawList& dl = *g.current->dl;
    switch (dir) {
    case 0: dl.AddTriangleFilled({c.x - r * 0.6f, c.y - r}, {c.x + r * 0.8f, c.y}, {c.x - r * 0.6f, c.y + r}, color); break;
    case 1: dl.AddTriangleFilled({c.x - r, c.y - r * 0.6f}, {c.x + r, c.y - r * 0.6f}, {c.x, c.y + r * 0.8f}, color); break;
    case 2: dl.AddTriangleFilled({c.x + r * 0.6f, c.y - r}, {c.x - r * 0.8f, c.y}, {c.x + r * 0.6f, c.y + r}, color); break;
    default: dl.AddTriangleFilled({c.x - r, c.y + r * 0.6f}, {c.x + r, c.y + r * 0.6f}, {c.x, c.y - r * 0.8f}, color); break;
    }
}

void RenderCheckMark(UiState& g, Vec2 pos, Color color, float size) {
    const float thickness = std::max(size / 5.0f, 1.0f);
    size -= thickness * 0.5f;
    pos += Vec2(thickness * 0.25f, thickness * 0.25f);
    const float third = size / 3.0f;
    const float bx = pos.x + third, by = pos.y + size - third * 0.5f;
    DrawList& dl = *g.current->dl;
    dl.PathClear();
    dl.PathLineTo({bx - third, by - third});
    dl.PathLineTo({bx, by});
    dl.PathLineTo({bx + third * 2.0f, by - third * 2.0f});
    dl.PathStroke(color, thickness);
}

const char* FormatV(UiState& g, const char* format, va_list args) {
    const int n = std::vsnprintf(g.formatBuffer, sizeof(g.formatBuffer), format, args);
    if (n < 0) g.formatBuffer[0] = '\0';
    return g.formatBuffer;
}

const char* Format(UiState& g, const char* format, ...) {
    va_list args;
    va_start(args, format);
    const char* result = FormatV(g, format, args);
    va_end(args);
    return result;
}

namespace {

// A format with exactly one conversion and nothing printf-specific around it: literal prefix, then %d, %i, %f or
// %.Nf, then literal suffix ("%%" allowed in both). Flags, widths and every other conversion are left to printf.
struct SimpleFormat {
    std::string_view prefix, suffix;
    char conversion = 0;
    int precision = -1;
};

bool ParseSimpleFormat(const char* format, SimpleFormat& f) {
    const char* conv = format;
    for (; *conv; ++conv) {
        if (*conv != '%') continue;
        if (conv[1] != '%') break;
        ++conv;  // "%%"
    }
    if (!*conv) return false;
    f.prefix = std::string_view(format, size_t(conv - format));
    const char* p = conv + 1;
    if (*p == '.') {
        ++p;
        if (*p < '0' || *p > '9') return false;
        f.precision = 0;
        for (; *p >= '0' && *p <= '9'; ++p) {
            f.precision = f.precision * 10 + (*p - '0');
            if (f.precision > 30) return false;
        }
    }
    f.conversion = *p;
    if (f.conversion != 'f' && (f.conversion != 'd' && f.conversion != 'i')) return false;
    if (f.conversion != 'f' && f.precision >= 0) return false;  // "%.3d" pads with zeros: printf's job
    f.suffix = std::string_view(p + 1);
    for (const char* s = p + 1; *s; ++s) {
        if (*s != '%') continue;
        if (s[1] != '%') return false;
        ++s;
    }
    return true;
}

// Copies literal format text, "%%" becoming '%'. False when it does not fit.
bool AppendLiteral(char*& out, const char* end, std::string_view text) {
    for (size_t i = 0; i < text.size(); ++i) {
        if (out == end) return false;
        *out++ = text[i];
        if (text[i] == '%') ++i;
    }
    return true;
}

template <typename Convert>
const char* FormatSimple(UiState& g, const SimpleFormat& f, Convert convert) {
    char* out = g.formatBuffer;
    const char* end = g.formatBuffer + sizeof(g.formatBuffer) - 1;
    if (!AppendLiteral(out, end, f.prefix)) return nullptr;
    const std::to_chars_result r = convert(out, end);
    if (r.ec != std::errc()) return nullptr;
    out = r.ptr;
    if (!AppendLiteral(out, end, f.suffix)) return nullptr;
    *out = '\0';
    return g.formatBuffer;
}

} // namespace

const char* FormatNumber(UiState& g, const char* format, double value) {
    SimpleFormat f;
    if (ParseSimpleFormat(format, f) && f.conversion == 'f') {
        const int precision = f.precision < 0 ? 6 : f.precision;
        const char* text = FormatSimple(g, f, [&](char* out, const char* end) {
            return std::to_chars(out, const_cast<char*>(end), value, std::chars_format::fixed, precision);
        });
        if (text) return text;
    }
    return Format(g, format, value);
}

const char* FormatNumber(UiState& g, const char* format, int value) {
    SimpleFormat f;
    if (ParseSimpleFormat(format, f) && f.conversion != 'f') {
        const char* text = FormatSimple(g, f, [&](char* out, const char* end) {
            return std::to_chars(out, const_cast<char*>(end), value);
        });
        if (text) return text;
    }
    return Format(g, format, value);
}

} // namespace ui_detail

// =====================================================================================================================
// Ui: setup and frame
// =====================================================================================================================
Ui::Ui() : m(std::make_unique<UiState>()) {
    for (float& d : m->keysDownDuration) d = -1.0f;
    for (float& d : m->keysDownDurationPrev) d = -1.0f;
}

Ui::~Ui() = default;

UiStyle& Ui::Style() { return m->style; }
UiInput& Ui::Input() { return m->input; }
UiPlatform& Ui::Platform() { return m->platform; }
const UiOutput& Ui::Output() const { return m->output; }
uint32_t Ui::FrameCount() const { return uint32_t(m->frameCount); }
double Ui::Time() const { return m->time; }
Vec2 Ui::GetDisplaySize() const { return m->displaySize; }

void Ui::NewFrame() {
    UiState& g = *m;
    DZ_ASSERT(!g.withinFrame && "NewFrame called twice without Render");
    DZ_ASSERT(g.style.font && "set UiStyle::font before the first frame");
    g.withinFrame = true;
    g.frameCount++;
    const float dt = g.input.deltaTime > 0.0f ? g.input.deltaTime : 1e-4f;
    g.time += dt;
    g.font = g.style.font;
    g.fontSize = g.style.fontSize;
    g.disabled = 0;
    UpdateAlpha(g);

    g.displaySize = g.input.displaySize;
    g.mousePos = g.input.mousePos;
    const Rect displayRect(Vec2(), g.displaySize);

    // ---- Mouse and keyboard edges
    const Vec2 mouse = g.mousePos;
    g.mouseDelta = (IsMouseValid(mouse) && IsMouseValid(g.mousePosPrev)) ? mouse - g.mousePosPrev : Vec2();
    g.mousePosPrev = mouse;
    for (int b = 0; b < kMouseButtons; ++b) {
        const bool down = g.input.mouseDown[b];
        g.mouseClicked[b] = down && !g.mouseDownPrev[b];
        g.mouseReleased[b] = !down && g.mouseDownPrev[b];
        g.mouseDoubleClicked[b] = false;
        if (g.mouseClicked[b]) {
            if (g.time - g.mouseClickedTime[b] < kDoubleClickTime &&
                LengthSq(mouse - g.mouseClickedPos[b]) < kDoubleClickMaxDist * kDoubleClickMaxDist) {
                g.mouseDoubleClicked[b] = true;
                g.mouseClickedTime[b] = -1e9;  // a third click starts over
            } else {
                g.mouseClickedTime[b] = g.time;
            }
            g.mouseClickedPos[b] = mouse;
        }
        g.mouseDownPrev[b] = down;
    }
    for (int k = 0; k < kKeyCount; ++k) {
        g.keysDownDurationPrev[k] = g.keysDownDuration[k];
        const float d = g.keysDownDuration[k];
        g.keysDownDuration[k] = g.input.keysDown[k] ? (d < 0.0f ? 0.0f : d + dt) : -1.0f;
    }

    // ---- Items: drop the active widget if it was not submitted last frame
    g.hoveredIdPrev = g.hoveredId;
    g.hoveredId = 0;
    g.popupClosedByClick = 0;
    g.activeIdPrevFrame = g.activeId;
    if (g.movingWindow) {
        if (g.input.mouseDown[0] && g.activeId == MoveId(g.movingWindow)) {
            g.activeIdAlive = true;
            g.movingWindow->pos += g.mouseDelta;
        } else {
            if (g.activeId == MoveId(g.movingWindow)) ClearActiveID(g);
            g.movingWindow = nullptr;
        }
    }
    if (g.activeId != 0 && !g.activeIdAlive) ClearActiveID(g);
    g.activeIdAlive = false;
    g.activeIdJustActivated = false;

    // ---- Windows (their `active` flags still describe last frame here)
    UpdateHoveredWindow(g);
    for (size_t i = 0; i < g.openPopups.size(); ++i) {
        const PopupData& p = g.openPopups[i];
        if (p.openFrame < g.frameCount - 1 && (!p.window || !p.window->active)) {
            ClosePopupToLevel(g, i);  // not submitted last frame
            break;
        }
    }
    if (g.mouseClicked[0] || g.mouseClicked[1]) {
        const bool modalOpen = std::any_of(g.openPopups.begin(), g.openPopups.end(),
                                           [](const PopupData& p) { return p.modal; });
        if (g.hoveredWindow || !modalOpen) {
            ClosePopupsOverWindow(g, g.hoveredWindow);
            FocusWindow(g, g.hoveredWindow);
        }
    }
    if (IsKeyPressedImpl(g, Key::Escape, false) && !g.openPopups.empty() && !g.openPopups.back().modal &&
        g.activeId == 0) {
        ClosePopupToLevel(g, g.openPopups.size() - 1);
    }
    if (g.input.mouseWheel != 0.0f && g.hoveredWindowInner) {
        Window* w = g.hoveredWindowInner;
        while (w && ((w->flags & WindowFlags::NoScrollWithMouse) || w->scrollMax.y <= 0.0f)) w = w->parent;
        if (w) {
            const float viewHeight = w->innerRect.Height();
            const float step = std::floor(std::min(5.0f * g.fontSize, 0.67f * viewHeight));
            w->scrollTargetY = Clamp(w->scroll.y - g.input.mouseWheel * step, 0.0f, w->scrollMax.y);
            w->scrollTargetCenterRatio = 0.0f;
        }
    }
    for (const auto& w : g.windows) {
        w->wasActive = w->active;
        w->active = false;
    }
    g.beginOrderCounter = 0;
    g.windowStack.clear();
    g.current = nullptr;
    g.tooltipDepth = 0;
    g.cursorRequest = MouseCursor::Arrow;
    g.tableStack.clear();
    g.tableListsUsed = 0;
    g.background.Reset(displayRect);
    g.foreground.Reset(displayRect);

    g.output.wantCaptureMouse = g.hoveredWindow != nullptr || g.activeId != 0;
    g.output.wantCaptureKeyboard = g.output.wantTextInput = g.activeId != 0 && g.activeId == g.inputText.id;
}

const DrawData& Ui::Render() {
    UiState& g = *m;
    DZ_ASSERT(g.withinFrame && "Render called without NewFrame");
    DZ_ASSERT(g.windowStack.empty() && "a Begin() is missing its End()");
    DZ_ASSERT(g.beginPopupStack.empty() && "a BeginPopup() is missing its EndPopup()");
    DZ_ASSERT(g.tableStack.empty() && "a BeginTable() is missing its EndTable()");

    // A click on a window's empty area (no widget took it) starts dragging the window.
    if (g.mouseClicked[0] && g.hoveredWindow && g.activeId == 0 && g.hoveredId == 0) {
        Window* root = g.hoveredWindow;
        const bool popupNonModal = (root->flags & kWindowPopup) && !(root->flags & kWindowModal);
        if (!(root->flags & WindowFlags::NoMove) && !popupNonModal) {
            g.movingWindow = root;
            SetActiveID(g, MoveId(root), root);
            g.activeIdClickOffset = g.mousePos - root->pos;
        }
    }

    g.output.cursor = g.cursorRequest;
    g.output.wantCaptureMouse = g.hoveredWindow != nullptr || g.activeId != 0;
    g.output.wantCaptureKeyboard = g.output.wantTextInput = g.activeId != 0 && g.activeId == g.inputText.id;
    RenderNotifications(g);  // over everything, in the foreground list; hovering one keeps the mouse
    g.input.characters.clear();
    g.input.mouseWheel = g.input.mouseWheelH = 0.0f;

    // Back to front: background, windows in focus order, popups in open order, tooltips, foreground.
    g.renderLists.clear();
    if (g.background.PrimCount()) g.renderLists.push_back(&g.background);
    for (Window* w : g.focusOrder) {
        if (w->active && !w->hidden) g.renderLists.push_back(&w->drawList);
    }
    for (const PopupData& p : g.openPopups) {
        if (p.window && p.window->active && !p.window->hidden) g.renderLists.push_back(&p.window->drawList);
    }
    for (const auto& w : g.windows) {
        if ((w->flags & kWindowTooltip) && w->active && !w->hidden) g.renderLists.push_back(&w->drawList);
    }
    if (g.foreground.PrimCount()) g.renderLists.push_back(&g.foreground);
    g.drawData.lists = g.renderLists.data();
    g.drawData.listCount = uint32_t(g.renderLists.size());
    g.drawData.displaySize = g.input.displaySize;  // the target, in pixels

    // The per-frame scratch buffers held caller data (formatted slider/label text, the password mask). Every glyph is
    // already baked into the draw lists above, so nothing still points at these - wipe the plaintext now rather than
    // leave this frame's text resident until the next one overwrites it.
    detail::ObfWipe(g.formatBuffer, sizeof(g.formatBuffer));
    if (!g.scratchText.empty()) {
        detail::ObfWipe(g.scratchText.data(), static_cast<unsigned>(g.scratchText.size()));
        g.scratchText.clear();
    }

    g.withinFrame = false;
    return g.drawData;
}

DrawList& Ui::WindowDrawList() {
    DZ_ASSERT(m->current);
    return *m->current->dl;
}
DrawList& Ui::BackgroundDrawList() { return m->background; }
DrawList& Ui::ForegroundDrawList() { return m->foreground; }

// =====================================================================================================================
// Windows
// =====================================================================================================================
bool Ui::Begin(std::string_view name, bool* open, uint32_t flags) {
    UiState& g = *m;
    DZ_ASSERT(g.withinFrame && "Begin must be called between NewFrame and Render");
    DZ_ASSERT(!name.empty());
    const UiStyle& style = g.style;
    const ID id = HashStr(name, 0);
    Window* w = FindWindow(g, id);
    const bool created = w == nullptr;
    if (created) {
        w = CreateWindowImpl(g, name, id, flags);
        if (!(flags & (kWindowChild | kWindowTooltip))) g.focusedWindow = w;  // new windows open in front, focused
    }
    Window* parent = (flags & kWindowChild) ? g.current : nullptr;
    g.windowStack.push_back(w);
    g.current = w;

    if (w->lastFrameActive == g.frameCount) {
        // Begin() again on a window already submitted this frame: append to it.
        w->dl->PushClipRect(w->clipRect, false);
        g.nextWindow.Clear();
        return !w->skipItems;
    }

    const bool isChild = (flags & kWindowChild) != 0;
    const bool isPopup = (flags & kWindowPopup) != 0;
    const bool isModal = (flags & kWindowModal) != 0;
    const bool isTooltip = (flags & kWindowTooltip) != 0;
    w->flags = flags;
    w->appearing = !w->wasActive;
    w->active = true;
    w->lastFrameActive = g.frameCount;
    w->beginOrder = g.beginOrderCounter++;
    w->parent = parent;
    w->root = parent ? parent->root : w;
    w->dl = w->root == w ? &w->drawList : w->root->dl;
    const Rect displayRect(Vec2(), g.displaySize);
    if (w->root == w) {
        w->drawList.Reset(displayRect);
    }
    if (w->appearing) {
        w->setPosAllowed |= CondBit(Cond::Appearing);
        w->setSizeAllowed |= CondBit(Cond::Appearing);
        w->setCollapsedAllowed |= CondBit(Cond::Appearing);
    }
    w->hidden = (parent && parent->hidden) || w->hiddenFrames > 0;
    if (w->hiddenFrames > 0) w->hiddenFrames--;

    // ---- Apply persisted layout (SaveIniSettings) on first creation, unless the caller set it explicitly.
    if (created && !(flags & (kWindowChild | kWindowPopup | kWindowTooltip | WindowFlags::NoSavedSettings))) {
        const auto it = g.windowSettings.find(id);
        if (it != g.windowSettings.end()) {
            const WindowSettings& s = it->second;
            if (s.hasPos && !g.nextWindow.hasPos) {
                g.nextWindow.hasPos = true;
                g.nextWindow.pos = s.pos;
                g.nextWindow.posCond = Cond::FirstUseEver;
            }
            if (s.hasSize && !g.nextWindow.hasSize && !(flags & WindowFlags::AlwaysAutoResize)) {
                g.nextWindow.hasSize = true;
                g.nextWindow.size = s.size;
                g.nextWindow.sizeCond = Cond::FirstUseEver;
            }
            if (!g.nextWindow.hasCollapsed) {
                g.nextWindow.hasCollapsed = true;
                g.nextWindow.collapsed = s.collapsed;
                g.nextWindow.collapsedCond = Cond::FirstUseEver;
            }
        }
    }

    // ---- Settings from SetNextWindow...()
    NextWindowData next = g.nextWindow;
    g.nextWindow.Clear();
    bool posFromNext = false, sizeFromNext = false;
    if (next.hasPos && CondAllowed(w->setPosAllowed, next.posCond)) {
        w->pos = next.pos;
        posFromNext = true;
    }
    if (next.hasSize && CondAllowed(w->setSizeAllowed, next.sizeCond)) {
        if (next.size.x > 0.0f) w->sizeFull.x = next.size.x;
        if (next.size.y > 0.0f) w->sizeFull.y = next.size.y;
        sizeFromNext = true;
    }
    if (next.hasCollapsed && CondAllowed(w->setCollapsedAllowed, next.collapsedCond)) w->collapsed = next.collapsed;
    if (next.hasFocus) FocusWindow(g, w);
    if (isChild) w->childBorder = next.childBorder;
    if (isPopup || isTooltip || isChild) w->collapsed = false;

    // ---- Size
    w->padding = isChild ? (w->childBorder ? style.windowPadding : Vec2()) : style.windowPadding;
    w->titleBarHeight = (flags & WindowFlags::NoTitleBar) ? 0.0f : FrameHeight(g);
    w->menuBarHeight = (flags & WindowFlags::MenuBar) ? FrameHeight(g) : 0.0f;
    w->borderSize = (flags & WindowFlags::NoBackground) ? 0.0f
                  : isChild                             ? (w->childBorder ? 1.0f : 0.0f)
                                                        : style.windowBorderSize;
    const bool autoResize = (flags & WindowFlags::AlwaysAutoResize) != 0;
    if (created && !sizeFromNext && !isChild) {
        w->autoFitFrames = 2;  // first frame lays out invisibly to measure, second applies the measured size
        w->hidden = true;
    }
    const bool fixedItemWidth = autoResize || w->hidden;
    const Vec2 fit = CalcAutoFitSize(g, w, next.maxHeight);
    const bool fitX = autoResize ? !(sizeFromNext && next.size.x > 0.0f) : (w->autoFitFrames > 0 || (sizeFromNext && next.size.x <= 0.0f));
    const bool fitY = autoResize ? !(sizeFromNext && next.size.y > 0.0f) : (w->autoFitFrames > 0 || (sizeFromNext && next.size.y <= 0.0f));
    if (fitX) w->sizeFull.x = fit.x;
    if (fitY) w->sizeFull.y = fit.y;
    if (w->autoFitFrames > 0) w->autoFitFrames--;
    if (!isChild && !isPopup && !isTooltip) w->sizeFull = MaxV(w->sizeFull, style.windowMinSize);

    // ---- Position
    const Vec2 pivot = next.pivot;
    auto applySize = [&] { w->size = w->collapsed ? Vec2(w->sizeFull.x, w->titleBarHeight) : w->sizeFull; };
    applySize();
    if (posFromNext && (pivot.x != 0.0f || pivot.y != 0.0f)) {
        // The size of an auto-fitting window settles over the next frames; keep re-centering until it has.
        w->pivotAnchor = next.pos;
        w->pivot = pivot;
        w->pivotFrames = 3;
    }
    if (w->pivotFrames > 0) {
        w->pos = w->pivotAnchor - w->size * w->pivot;
        w->pivotFrames--;
    }
    if (posFromNext && next.hasAnchor) {
        // Below the anchor (e.g. a combo box), or above it when there is no room below.
        if (w->pos.y + w->size.y > g.displaySize.y && next.anchor.min.y - w->size.y >= 0.0f) {
            w->pos.y = next.anchor.min.y - w->size.y;
        }
    }
    if ((isPopup || isTooltip) && g.displaySize.x > 0.0f) {
        w->pos = MaxV(Vec2(), MinV(w->pos, g.displaySize - w->size));
    } else if (!isChild && g.displaySize.x > 0.0f) {
        // Keep enough of a top-level window on screen to grab it again.
        const float grab = std::min(40.0f, w->size.x);
        w->pos.x = Clamp(w->pos.x, grab - w->size.x, g.displaySize.x - grab);
        w->pos.y = Clamp(w->pos.y, 0.0f, std::max(0.0f, g.displaySize.y - std::max(w->titleBarHeight, 20.0f)));
    }
    w->pos = {std::floor(w->pos.x), std::floor(w->pos.y)};

    // Window decorations are hit-tested against the whole window.
    w->outerRect = Rect::FromPosSize(w->pos, w->size);
    w->clipRect = parent ? w->outerRect.Intersect(parent->clipRect) : w->outerRect;

    // ---- Resizing: drag the grip in the bottom-right corner, any edge, or any corner
    const bool resizable = !(flags & (WindowFlags::NoResize | WindowFlags::AlwaysAutoResize)) && !isChild &&
                           !isPopup && !isTooltip && !w->collapsed;
    w->resizable = resizable;
    const float gripSize = std::floor(std::max(g.fontSize * 1.1f, style.windowRounding + 1.0f + g.fontSize * 0.2f));
    Color gripColor = 0;
    uint32_t resizeEdges = 0;  // edges under the mouse or being dragged, highlighted when the frame is drawn
    bool resizeHeld = false;
    if (resizable && !w->hidden) {
        const ID resizeId = HashStr(DZ_STR("#RESIZE"), w->id);
        const Vec2 mouse = g.mousePos;
        const Rect grip(w->outerRect.max - Vec2(gripSize, gripSize), w->outerRect.max);
        if (g.hoveredWindow == w && g.hoveredId == 0 && g.disabled == 0 && (g.activeId == 0 || g.activeId == resizeId)) {
            resizeEdges = grip.Contains(mouse) ? (kEdgeRight | kEdgeBottom) : ResizeEdgesAt(w->outerRect, mouse, gripSize);
        }
        if (resizeEdges) {
            g.hoveredId = resizeId;  // the border wins over the scrollbar and title bar beneath it
            if (g.mouseClicked[0]) {
                if (g.mouseDoubleClicked[0] && grip.Contains(mouse)) {
                    w->sizeFull = CalcAutoFitSize(g, w, 0.0f);  // double-click the grip: fit the content
                } else {
                    SetActiveID(g, resizeId, w);
                    g.resizeEdges = resizeEdges;
                    g.resizeStartRect = Rect::FromPosSize(w->pos, w->sizeFull);
                    g.resizeStartMouse = mouse;
                }
            }
        }
        if (g.activeId == resizeId) {
            if (g.input.mouseDown[0]) {
                KeepAliveID(g, resizeId);
                resizeHeld = true;
                resizeEdges = g.resizeEdges;
                if (IsMouseValid(mouse)) {
                    // The grabbed edges follow the mouse, the opposite ones stay put.
                    const Vec2 d = mouse - g.resizeStartMouse;
                    const Vec2 minSize = style.windowMinSize;
                    Rect r = g.resizeStartRect;
                    if (resizeEdges & kEdgeLeft) r.min.x = std::min(r.min.x + d.x, r.max.x - minSize.x);
                    if (resizeEdges & kEdgeRight) r.max.x = std::max(r.max.x + d.x, r.min.x + minSize.x);
                    if (resizeEdges & kEdgeTop) r.min.y = std::min(std::max(r.min.y + d.y, 0.0f), r.max.y - minSize.y);
                    if (resizeEdges & kEdgeBottom) r.max.y = std::max(r.max.y + d.y, r.min.y + minSize.y);
                    w->pos = {std::floor(r.min.x), std::floor(r.min.y)};
                    w->sizeFull = {std::floor(r.max.x) - w->pos.x, std::floor(r.max.y) - w->pos.y};
                    w->autoFitFrames = 0;
                }
            } else {
                ClearActiveID(g);
            }
        }
        if (resizeEdges) g.cursorRequest = ResizeCursor(resizeEdges);
        applySize();
        w->outerRect = Rect::FromPosSize(w->pos, w->size);
        w->clipRect = w->outerRect;
        const bool onGrip = resizeEdges == (kEdgeRight | kEdgeBottom);
        gripColor = StyleColor(g, onGrip && resizeHeld ? UiColor::ResizeGripActive
                                  : onGrip             ? UiColor::ResizeGripHovered
                                                       : UiColor::ResizeGrip);
    }

    // ---- Scrolling
    const float borderSize = w->borderSize;
    const float viewHeight = w->size.y - std::max(w->titleBarHeight, borderSize) - borderSize - w->menuBarHeight;
    const float contentHeight = w->contentSize.y + w->padding.y * 2.0f;
    w->scrollbarY = !w->collapsed && !(flags & WindowFlags::NoScrollbar) && contentHeight > viewHeight + 0.5f;
    w->scrollMax.y = std::max(0.0f, contentHeight - viewHeight);
    if (w->scrollTargetY >= 0.0f) {
        w->scroll.y = w->scrollTargetY - w->scrollTargetCenterRatio * viewHeight;
        w->scrollTargetY = -1.0f;
    }
    w->scroll.y = std::floor(Clamp(w->scroll.y, 0.0f, w->scrollMax.y));
    const float scrollbarW = w->scrollbarY ? style.scrollbarSize : 0.0f;
    const float topChrome = std::max(w->titleBarHeight, borderSize) + w->menuBarHeight;
    w->innerRect = Rect(w->pos.x + borderSize, w->pos.y + topChrome,
                        w->pos.x + w->size.x - borderSize - scrollbarW, w->pos.y + w->size.y - borderSize);
    if (w->menuBarHeight > 0.0f && !w->collapsed) {
        const float top = w->pos.y + std::max(w->titleBarHeight, borderSize);
        w->menuBarRect = Rect(w->pos.x + borderSize, top, w->pos.x + w->size.x - borderSize, top + w->menuBarHeight);
    } else {
        w->menuBarRect = Rect();
    }
    if (w->collapsed) w->innerRect.max.y = w->innerRect.min.y;

    // ---- Title bar
    const bool focused = g.focusedWindow == w->root;
    const Rect title(w->pos, w->pos + Vec2(w->size.x, w->titleBarHeight));
    const float rounding = (isPopup || isTooltip) ? style.popupRounding : (isChild ? style.frameRounding : style.windowRounding);
    Rect closeRect, collapseRect;
    bool closeHovered = false, closeHeld = false, collapseHovered = false;
    if (w->titleBarHeight > 0.0f && !w->hidden) {
        const float buttonSize = g.fontSize;
        const float cy = title.Center().y;
        if (open) {
            closeRect = Rect::FromCenter({title.max.x - style.framePadding.x - buttonSize * 0.5f, cy},
                                         {buttonSize * 0.5f, buttonSize * 0.5f});
            if (ButtonBehavior(g, closeRect, HashStr(DZ_STR("#CLOSE"), w->id), &closeHovered, &closeHeld)) *open = false;
        }
        if (!(flags & WindowFlags::NoCollapse)) {
            collapseRect = Rect::FromCenter({title.min.x + style.framePadding.x + buttonSize * 0.5f, cy},
                                            {buttonSize * 0.5f, buttonSize * 0.5f});
            if (ButtonBehavior(g, collapseRect, HashStr(DZ_STR("#COLLAPSE"), w->id), &collapseHovered, nullptr)) {
                w->collapsed = !w->collapsed;
            }
            if (g.mouseDoubleClicked[0] && g.hoveredWindow == w && g.hoveredId == 0 &&
                title.Contains(g.mousePos)) {
                w->collapsed = !w->collapsed;
            }
        }
    }

    // ---- Scrollbar
    Rect track, grab;
    Color grabColor = 0;
    if (w->scrollbarY && !w->hidden) {
        track = Rect(w->innerRect.max.x, w->innerRect.min.y, w->innerRect.max.x + scrollbarW, w->innerRect.max.y);
        const float trackH = track.Height();
        const float grabH = Clamp(trackH * viewHeight / std::max(contentHeight, 1.0f), style.grabMinSize, trackH);
        const float travel = std::max(trackH - grabH, 1.0f);
        float grabY = track.min.y + travel * (w->scrollMax.y > 0.0f ? w->scroll.y / w->scrollMax.y : 0.0f);
        const ID scrollId = HashStr(DZ_STR("#SCROLLY"), w->id);
        bool hovered, held;
        ButtonBehavior(g, track, scrollId, &hovered, &held);
        if (held) {
            if (g.activeIdJustActivated) {
                const bool onGrab = g.mousePos.y >= grabY && g.mousePos.y < grabY + grabH;
                g.activeIdClickOffset.y = onGrab ? g.mousePos.y - grabY : grabH * 0.5f;
            }
            const float t = Clamp((g.mousePos.y - g.activeIdClickOffset.y - track.min.y) / travel, 0.0f, 1.0f);
            w->scroll.y = std::floor(t * w->scrollMax.y);
            grabY = track.min.y + travel * t;
        }
        grab = Rect(track.min.x + 2.0f, grabY + 2.0f, track.max.x - 2.0f, grabY + grabH - 2.0f);
        grabColor = StyleColor(g, held ? UiColor::ScrollbarGrabActive
                                  : hovered ? UiColor::ScrollbarGrabHovered
                                            : UiColor::ScrollbarGrab);
    }

    // ---- Draw the frame
    DrawList& dl = *w->dl;
    if (!w->hidden) {
        if (isModal) dl.AddRectFilled(displayRect, StyleColor(g, UiColor::ModalDimBg));
        const bool background = !(flags & WindowFlags::NoBackground);
        const bool castsShadow = background && !isChild && !(flags & WindowFlags::NoShadow);
        if (castsShadow && style.windowShadowSize > 0.0f) {
            // Cut out under the window: only the visible rim is shaded, not the whole window area again.
            dl.AddShadow(w->outerRect, StyleColor(g, UiColor::WindowShadow), style.windowShadowSize, rounding,
                         {0.0f, style.windowShadowSize * 0.25f}, true);
        }
        if (castsShadow && style.glowSize > 0.0f && (focused || isPopup)) {
            dl.AddShadow(w->outerRect, StyleColor(g, UiColor::Glow, 0.4f), style.glowSize * 1.6f, rounding, {}, true);
        }
        if (castsShadow && (style.hardShadow.x != 0.0f || style.hardShadow.y != 0.0f)) {
            dl.AddRectFilled(w->outerRect.Translated(style.hardShadow), StyleColor(g, UiColor::WindowShadow), rounding);
        }
        // UiStyle::gradient shades window bodies and title bars at half the strength it shades widgets.
        const float shade = Clamp(style.gradient, 0.0f, 1.0f) * 0.2f;
        auto shadeFill = [&](RectStyle& rs, Color fill) {
            rs.fill = fill;
            if (shade <= 0.0f) return;
            rs.fill = LerpColor(fill, WithAlpha(colors::White, ColorAlpha(fill)), shade);
            rs.fillEnd = LerpColor(fill, WithAlpha(colors::Black, ColorAlpha(fill)), shade);
            rs.gradient = Gradient::Vertical;
        };
        // An open window draws background and border as one prim (one pass over its pixels) and keeps the title and
        // menu bars inside the border; a collapsed one is just its title bar with the border over it.
        const bool border = background && borderSize > 0.0f;
        const bool bodyWithBorder = background && !w->collapsed;
        if (bodyWithBorder) {
            RectStyle body;
            shadeFill(body, StyleColor(g, (isPopup || isTooltip) && !isModal ? UiColor::PopupBg
                                          : isChild                            ? UiColor::ChildBg
                                                                               : UiColor::WindowBg));
            body.radii = rounding;
            if (border) {
                body.borderWidth = borderSize;
                body.borderColor = StyleColor(g, UiColor::Border);
            }
            dl.AddRectEx(w->outerRect, body);
        }
        const float inset = bodyWithBorder && border ? borderSize : 0.0f;
        const float innerRounding = std::max(rounding - inset, 0.0f);
        if (w->titleBarHeight > 0.0f) {
            const float bottom = w->collapsed ? rounding : 0.0f;
            const Rect bar(title.min.x + inset, title.min.y + inset, title.max.x - inset, title.max.y);
            const CornerRadii barRadii(innerRounding, innerRounding, bottom, bottom);
            const TitleShape shape = style.titleShape;
            const bool solid = shape == TitleShape::Solid && focused;
            Color titleText = StyleColor(g, UiColor::Text);
            if (shape == TitleShape::Plain) {
                // The title sits on the window background, over a separator (a collapsed window keeps a bar).
                if (w->collapsed) dl.AddRectFilled(bar, StyleColor(g, UiColor::TitleBg), barRadii);
                else dl.AddRectFilled(Rect(bar.min.x, bar.max.y - 1.0f, bar.max.x, bar.max.y), StyleColor(g, UiColor::Separator));
                if (!focused) titleText = StyleColor(g, UiColor::TextDisabled);
            } else {
                RectStyle rs;
                shadeFill(rs, solid ? StyleColor(g, UiColor::CheckMark)
                                    : StyleColor(g, focused ? UiColor::TitleBgActive : UiColor::TitleBg));
                rs.radii = barRadii;
                if (rs.gradient == Gradient::None) dl.AddRectFilled(bar, rs.fill, barRadii);
                else dl.AddRectEx(bar, rs);
                if (solid) titleText = StyleColor(g, UiColor::AccentText);
                if (shape == TitleShape::Accent) {
                    const Rect stripe(bar.min.x, bar.min.y, bar.max.x, bar.min.y + 2.0f);
                    if (focused) RenderGlow(g, stripe, 0.0f, 0.8f);
                    dl.AddRectFilled(stripe, focused ? StyleColor(g, UiColor::CheckMark) : StyleColor(g, UiColor::Border),
                                     CornerRadii(innerRounding, innerRounding, 0.0f, 0.0f));
                }
            }
            float textMinX = title.min.x + style.framePadding.x;
            float textMaxX = title.max.x - style.framePadding.x;
            if (collapseRect.Width() > 0.0f) {
                if (collapseHovered) {
                    dl.AddCircleFilled(collapseRect.Center(), g.fontSize * 0.5f + 1.0f,
                                       solid ? ScaleAlpha(titleText, 0.25f) : StyleColor(g, UiColor::ButtonHovered));
                }
                RenderArrow(g, collapseRect.Center(), g.fontSize * 0.55f, w->collapsed ? 0 : 1, titleText);
                textMinX = collapseRect.max.x + style.itemInnerSpacing.x;
            }
            if (closeRect.Width() > 0.0f) {
                if (closeHovered) {
                    dl.AddCircleFilled(closeRect.Center(), g.fontSize * 0.5f + 1.0f,
                                       solid ? ScaleAlpha(titleText, closeHeld ? 0.4f : 0.25f)
                                             : StyleColor(g, closeHeld ? UiColor::ButtonActive : UiColor::ButtonHovered));
                }
                const Vec2 c = closeRect.Center();
                const float e = g.fontSize * 0.22f;
                dl.AddLine(c - Vec2(e, e), c + Vec2(e, e), titleText, 1.5f, LineCap::Round);
                dl.AddLine(c + Vec2(e, -e), c + Vec2(-e, e), titleText, 1.5f, LineCap::Round);
                textMaxX = closeRect.min.x - style.itemInnerSpacing.x;
            }
            const Rect textRect(textMinX, title.min.y, textMaxX, title.max.y);
            const auto nm = w->name.decode();
            RenderTextAligned(g, textRect, VisibleText(nm.view()), {0.0f, 0.5f}, &textRect, nullptr, titleText);
        }
        if (w->menuBarHeight > 0.0f && !w->collapsed) {
            const float top = w->titleBarHeight > 0.0f ? 0.0f : innerRounding;
            dl.AddRectFilled(w->menuBarRect, StyleColor(g, UiColor::MenuBarBg), CornerRadii(top, top, 0.0f, 0.0f));
        }
        if (border && !bodyWithBorder) dl.AddRect(w->outerRect, StyleColor(g, UiColor::Border), rounding, borderSize);
        if (w->scrollbarY) {
            dl.AddRectFilled(track, StyleColor(g, UiColor::ScrollbarBg));
            dl.AddRectFilled(grab, grabColor, style.scrollbarRounding);
        }
        if (gripColor) {
            const Vec2 c = w->outerRect.max - Vec2(borderSize + 1.0f, borderSize + 1.0f);
            const float s = gripSize * 0.75f;
            dl.AddTriangleFilled({c.x, c.y - s}, c, {c.x - s, c.y}, gripColor);
        }
        if (resizeEdges && resizeEdges != (kEdgeRight | kEdgeBottom)) {
            // Light up the straight part of the grabbed edges (the bottom-right corner lights up its grip instead).
            const Color c = StyleColor(g, resizeHeld ? UiColor::ResizeGripActive : UiColor::ResizeGripHovered);
            const Rect& r = w->outerRect;
            if (resizeEdges & kEdgeLeft) dl.AddLine({r.min.x + 1.0f, r.min.y + rounding}, {r.min.x + 1.0f, r.max.y - rounding}, c, 2.0f);
            if (resizeEdges & kEdgeRight) dl.AddLine({r.max.x - 1.0f, r.min.y + rounding}, {r.max.x - 1.0f, r.max.y - rounding}, c, 2.0f);
            if (resizeEdges & kEdgeTop) dl.AddLine({r.min.x + rounding, r.min.y + 1.0f}, {r.max.x - rounding, r.min.y + 1.0f}, c, 2.0f);
            if (resizeEdges & kEdgeBottom) dl.AddLine({r.min.x + rounding, r.max.y - 1.0f}, {r.max.x - rounding, r.max.y - 1.0f}, c, 2.0f);
        }
    }

    // ---- Layout for the window's content
    w->clipRect = parent ? w->innerRect.Intersect(parent->clipRect) : w->innerRect;
    const Vec2 contentMin = w->innerRect.min + w->padding;
    w->contentStartX = std::floor(contentMin.x);
    w->cursorStartPos = {std::floor(contentMin.x), std::floor(contentMin.y - w->scroll.y)};
    w->cursorPos = w->cursorStartPos;
    w->cursorMaxPos = w->cursorStartPos;
    w->cursorPosPrevLine = w->cursorPos;
    w->contentRegionMax = w->innerRect.max - w->padding;
    w->currLineHeight = w->prevLineHeight = 0.0f;
    w->currLineTextBaseOffset = w->prevLineTextBaseOffset = 0.0f;
    w->indent = 0.0f;
    // Auto-sized windows get a fixed default width: a width relative to the window would feed back into its size.
    w->itemWidth = fixedItemWidth ? std::floor(g.fontSize * 14.0f)
                                    : std::floor(std::max(1.0f, (w->contentRegionMax.x - w->contentStartX) * 0.65f));
    w->itemWidthStack.clear();
    w->groupStack.clear();
    w->idStack.assign(1, w->id);
    w->treeDepth = 0;
    w->lastItemId = 0;
    w->lastItemStatus = 0;
    w->lastItemRect = Rect();
    w->skipItems = w->collapsed;
    dl.PushClipRect(w->clipRect, false);

    w->setPosAllowed &= ~(CondBit(Cond::FirstUseEver) | CondBit(Cond::Appearing));
    w->setSizeAllowed &= ~(CondBit(Cond::FirstUseEver) | CondBit(Cond::Appearing));
    w->setCollapsedAllowed &= ~(CondBit(Cond::FirstUseEver) | CondBit(Cond::Appearing));
    return !w->skipItems;
}

void Ui::End() {
    UiState& g = *m;
    DZ_ASSERT(!g.windowStack.empty() && "End() without Begin()");
    Window* w = g.current;
    DZ_ASSERT(w->groupStack.empty() && "a BeginGroup() is missing its EndGroup()");
    w->dl->PopClipRect();
    w->contentSize = {std::floor(std::max(0.0f, w->cursorMaxPos.x - w->cursorStartPos.x)),
                      std::floor(std::max(0.0f, w->cursorMaxPos.y - w->cursorStartPos.y))};
    g.windowStack.pop_back();
    g.current = g.windowStack.empty() ? nullptr : g.windowStack.back();
}

bool Ui::BeginChild(std::string_view strId, Vec2 size, bool border, uint32_t flags) {
    UiState& g = *m;
    Window* parent = g.current;
    DZ_ASSERT(parent && "BeginChild must be inside a window");
    const ID id = HashStr(strId, parent->idStack.back());
    const Vec2 avail = parent->contentRegionMax - parent->cursorPos;
    Vec2 s = size;
    if (s.x <= 0.0f) s.x = std::max(avail.x + s.x, 4.0f);
    if (s.y <= 0.0f) s.y = std::max(avail.y + s.y, 4.0f);
    char name[512];
    const auto pn = parent->name.decode();
    std::snprintf(name, sizeof(name), DZ_STR("%s/%.*s_%08X"), pn.c_str(), int(strId.size()), strId.data(), id);
    g.nextWindow.hasPos = true;
    g.nextWindow.pos = parent->cursorPos;
    g.nextWindow.posCond = Cond::Always;
    g.nextWindow.pivot = {};
    g.nextWindow.hasSize = true;
    g.nextWindow.size = {std::floor(s.x), std::floor(s.y)};
    g.nextWindow.sizeCond = Cond::Always;
    g.nextWindow.childBorder = border;
    flags |= kWindowChild | WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove |
             WindowFlags::NoCollapse | WindowFlags::NoShadow;
    return Begin(name, nullptr, flags);
}

void Ui::EndChild() {
    UiState& g = *m;
    Window* child = g.current;
    DZ_ASSERT(child && (child->flags & kWindowChild) && "EndChild() without BeginChild()");
    const Vec2 size = child->size;
    const ID id = child->id;
    End();
    Window* parent = g.current;
    const Rect bb = Rect::FromPosSize(parent->cursorPos, size);
    ItemSize(g, size);
    ItemAdd(g, bb, id);
}

void Ui::SetNextWindowPos(Vec2 pos, Cond cond, Vec2 pivot) {
    m->nextWindow.hasPos = true;
    m->nextWindow.pos = pos;
    m->nextWindow.posCond = cond;
    m->nextWindow.pivot = pivot;
}

void Ui::SetNextWindowSize(Vec2 size, Cond cond) {
    m->nextWindow.hasSize = true;
    m->nextWindow.size = size;
    m->nextWindow.sizeCond = cond;
}

void Ui::SetNextWindowCollapsed(bool collapsed, Cond cond) {
    m->nextWindow.hasCollapsed = true;
    m->nextWindow.collapsed = collapsed;
    m->nextWindow.collapsedCond = cond;
}

void Ui::SetNextWindowFocus() { m->nextWindow.hasFocus = true; }
Vec2 Ui::GetWindowPos() const { return m->current->pos; }
Vec2 Ui::GetWindowSize() const { return m->current->size; }

bool Ui::IsWindowHovered() const {
    for (const Window* h = m->hoveredWindowInner; h; h = h->parent) {
        if (h == m->current) return true;
    }
    return false;
}

bool Ui::IsWindowFocused() const { return m->focusedWindow == m->current->root; }
bool Ui::IsWindowAppearing() const { return m->current->appearing; }
float Ui::GetScrollY() const { return m->current->scroll.y; }
float Ui::GetScrollMaxY() const { return m->current->scrollMax.y; }

void Ui::SetScrollY(float scrollY) {
    m->current->scrollTargetY = std::max(0.0f, scrollY);
    m->current->scrollTargetCenterRatio = 0.0f;
}

void Ui::SetScrollHereY(float centerRatio) {
    Window* w = m->current;
    const float localY = w->cursorPos.y - w->cursorStartPos.y + w->padding.y;
    w->scrollTargetY = std::max(0.0f, localY);
    w->scrollTargetCenterRatio = Clamp(centerRatio, 0.0f, 1.0f);
}

// =====================================================================================================================
// Layout
// =====================================================================================================================
void Ui::SameLine(float offsetFromStartX, float spacing) {
    Window* w = m->current;
    if (w->skipItems) return;
    if (offsetFromStartX != 0.0f) {
        w->cursorPos.x = w->contentStartX + offsetFromStartX + std::max(spacing, 0.0f);
    } else {
        w->cursorPos.x = w->cursorPosPrevLine.x + (spacing < 0.0f ? m->style.itemSpacing.x : spacing);
    }
    w->cursorPos.y = w->cursorPosPrevLine.y;
    w->currLineHeight = w->prevLineHeight;
    w->currLineTextBaseOffset = w->prevLineTextBaseOffset;
}

void Ui::NewLine() {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const float backup = w->currLineTextBaseOffset;
    w->currLineTextBaseOffset = 0.0f;
    ItemSize(g, {0.0f, w->currLineHeight > 0.0f ? 0.0f : TextLineHeight(g)});
    w->currLineTextBaseOffset = backup;
}

void Ui::Spacing() {
    if (!m->current->skipItems) ItemSize(*m, {});
}

void Ui::Dummy(Vec2 size) {
    UiState& g = *m;
    if (g.current->skipItems) return;
    const Rect bb = Rect::FromPosSize(g.current->cursorPos, size);
    ItemSize(g, size);
    ItemAdd(g, bb, 0);
}

void Ui::Separator() {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return;
    const Rect bb(w->cursorPos.x, w->cursorPos.y, w->contentRegionMax.x, w->cursorPos.y + 1.0f);
    ItemSize(g, {0.0f, 1.0f});
    if (ItemAdd(g, bb, 0)) w->dl->AddRectFilled(bb, StyleColor(g, UiColor::Separator));
}

void Ui::Indent(float width) {
    Window* w = m->current;
    w->indent += width > 0.0f ? width : m->style.indentSpacing;
    w->cursorPos.x = w->contentStartX + w->indent;
}

void Ui::Unindent(float width) {
    Window* w = m->current;
    w->indent -= width > 0.0f ? width : m->style.indentSpacing;
    w->cursorPos.x = w->contentStartX + w->indent;
}

void Ui::BeginGroup() {
    Window* w = m->current;
    w->groupStack.push_back({w->cursorPos, w->cursorMaxPos, w->indent, w->currLineHeight, w->currLineTextBaseOffset});
    w->indent = w->cursorPos.x - w->contentStartX;
    w->cursorMaxPos = w->cursorPos;
    w->currLineHeight = 0.0f;
}

void Ui::EndGroup() {
    UiState& g = *m;
    Window* w = g.current;
    DZ_ASSERT(!w->groupStack.empty() && "EndGroup() without BeginGroup()");
    const GroupData group = w->groupStack.back();
    w->groupStack.pop_back();
    const Rect bb(group.cursorPos, MaxV(w->cursorMaxPos, group.cursorPos));
    w->cursorPos = group.cursorPos;
    w->cursorMaxPos = MaxV(group.cursorMaxPos, bb.max);
    w->indent = group.indent;
    w->currLineHeight = group.currLineHeight;
    w->currLineTextBaseOffset = std::max(group.currLineTextBaseOffset, w->prevLineTextBaseOffset);
    ItemSize(g, bb.Size(), w->currLineTextBaseOffset);
    ItemAdd(g, bb, 0);
}

void Ui::PushItemWidth(float width) {
    Window* w = m->current;
    w->itemWidthStack.push_back(w->itemWidth);
    w->itemWidth = width == 0.0f ? std::floor((w->contentRegionMax.x - w->contentStartX) * 0.65f) : width;
}

void Ui::PopItemWidth() {
    Window* w = m->current;
    DZ_ASSERT(!w->itemWidthStack.empty());
    w->itemWidth = w->itemWidthStack.back();
    w->itemWidthStack.pop_back();
}

void Ui::SetNextItemWidth(float width) {
    m->nextItem.hasWidth = true;
    m->nextItem.width = width;
}

float Ui::CalcItemWidth() const { return ui_detail::CalcItemWidth(*m); }

void Ui::AlignTextToFramePadding() {
    Window* w = m->current;
    w->currLineHeight = std::max(w->currLineHeight, FrameHeight(*m));
    w->currLineTextBaseOffset = std::max(w->currLineTextBaseOffset, m->style.framePadding.y);
}

Vec2 Ui::GetContentRegionAvail() const { return m->current->contentRegionMax - m->current->cursorPos; }
Vec2 Ui::GetCursorPos() const { return m->current->cursorPos - m->current->pos + m->current->scroll; }

void Ui::SetCursorPos(Vec2 localPos) {
    Window* w = m->current;
    w->cursorPos = w->pos - w->scroll + localPos;
    w->cursorMaxPos = MaxV(w->cursorMaxPos, w->cursorPos);
}

Vec2 Ui::GetCursorScreenPos() const { return m->current->cursorPos; }

void Ui::SetCursorScreenPos(Vec2 pos) {
    Window* w = m->current;
    w->cursorPos = pos;
    w->cursorMaxPos = MaxV(w->cursorMaxPos, pos);
}

float Ui::GetTextLineHeight() const { return TextLineHeight(*m); }
float Ui::GetFrameHeight() const { return FrameHeight(*m); }
float Ui::GetFrameHeightWithSpacing() const { return FrameHeight(*m) + m->style.itemSpacing.y; }
Vec2 Ui::CalcTextSize(std::string_view text, float wrapWidth) const {
    return TextSize(*m, VisibleText(text), wrapWidth);
}

// =====================================================================================================================
// IDs
// =====================================================================================================================
void Ui::PushID(std::string_view id) {
    Window* w = m->current;
    w->idStack.push_back(HashStr(id, w->idStack.back()));
}

void Ui::PushID(int id) {
    Window* w = m->current;
    w->idStack.push_back(HashData(&id, sizeof(id), w->idStack.back()));
}

void Ui::PushID(const void* id) {
    Window* w = m->current;
    w->idStack.push_back(HashData(&id, sizeof(id), w->idStack.back()));
}

void Ui::PopID() {
    Window* w = m->current;
    DZ_ASSERT(w->idStack.size() > 1 && "PopID() without PushID()");
    w->idStack.pop_back();
}

uint32_t Ui::GetID(std::string_view id) const { return HashStr(id, m->current->idStack.back()); }

// =====================================================================================================================
// Style stacks
// =====================================================================================================================
void Ui::PushStyleColor(UiColor color, Color value) {
    m->colorStack.push_back({color, m->style.colors[size_t(color)]});
    m->style.colors[size_t(color)] = value;
}

void Ui::PopStyleColor(int count) {
    for (; count > 0 && !m->colorStack.empty(); --count) {
        const ColorMod mod = m->colorStack.back();
        m->colorStack.pop_back();
        m->style.colors[size_t(mod.color)] = mod.backup;
    }
}

void Ui::PushStyleVar(UiStyleVar var, float value) {
    const VarInfo& info = GetVarInfo(var);
    DZ_ASSERT(info.count == 1 && "this style variable is a Vec2");
    float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(&m->style) + info.offset);
    m->styleStack.push_back({var, {field[0], 0.0f}});
    field[0] = value;
    UpdateAlpha(*m);
}

void Ui::PushStyleVar(UiStyleVar var, Vec2 value) {
    const VarInfo& info = GetVarInfo(var);
    DZ_ASSERT(info.count == 2 && "this style variable is a float");
    float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(&m->style) + info.offset);
    m->styleStack.push_back({var, {field[0], field[1]}});
    field[0] = value.x;
    field[1] = value.y;
}

void Ui::PopStyleVar(int count) {
    for (; count > 0 && !m->styleStack.empty(); --count) {
        const StyleMod mod = m->styleStack.back();
        m->styleStack.pop_back();
        const VarInfo& info = GetVarInfo(mod.var);
        float* field = reinterpret_cast<float*>(reinterpret_cast<char*>(&m->style) + info.offset);
        for (int i = 0; i < info.count; ++i) field[i] = mod.backup[i];
    }
    UpdateAlpha(*m);
}

void Ui::PushFont(const Font* font, float size) {
    m->fontStack.push_back({m->font, m->fontSize});
    if (font) m->font = font;
    if (size > 0.0f) m->fontSize = size;
}

void Ui::PopFont() {
    DZ_ASSERT(!m->fontStack.empty() && "PopFont() without PushFont()");
    m->font = m->fontStack.back().font;
    m->fontSize = m->fontStack.back().size;
    m->fontStack.pop_back();
}

void Ui::BeginDisabled(bool disabled) {
    m->disabledStack.push_back(disabled ? 1.0f : 0.0f);
    if (disabled) m->disabled++;
    UpdateAlpha(*m);
}

void Ui::EndDisabled() {
    DZ_ASSERT(!m->disabledStack.empty() && "EndDisabled() without BeginDisabled()");
    if (m->disabledStack.back() != 0.0f) m->disabled--;
    m->disabledStack.pop_back();
    UpdateAlpha(*m);
}

Color Ui::GetColor(UiColor color, float alphaScale) const { return StyleColor(*m, color, alphaScale); }

// =====================================================================================================================
// Popups and tooltips
// =====================================================================================================================
void Ui::OpenPopup(std::string_view id) { OpenPopupEx(*m, GetID(id)); }

bool Ui::BeginPopup(std::string_view id, uint32_t flags) { return BeginPopupEx(*this, *m, GetID(id), flags); }

bool Ui::BeginPopupModal(std::string_view name, bool* open, uint32_t flags) {
    UiState& g = *m;
    const ID id = GetID(name);
    if (!IsPopupOpenId(g, id)) {
        g.nextWindow.Clear();
        return false;
    }
    const size_t level = g.beginPopupStack.size();
    g.openPopups[level].modal = true;
    if (!g.nextWindow.hasPos) SetNextWindowPos(g.displaySize * 0.5f, Cond::Appearing, {0.5f, 0.5f});
    flags |= kWindowPopup | kWindowModal | WindowFlags::NoCollapse;
    if (!g.nextWindow.hasSize) flags |= WindowFlags::AlwaysAutoResize;
    g.beginPopupStack.push_back(g.openPopups[level]);
    const bool visible = Begin(name, open, flags);
    if (level < g.openPopups.size()) g.openPopups[level].window = g.current;
    if (open && !*open) ClosePopupToLevel(g, level);
    if (!visible) {
        EndPopup();
        return false;
    }
    return true;
}

bool Ui::BeginPopupContextItem(std::string_view id) {
    UiState& g = *m;
    const ID popupId = id.empty() ? g.current->lastItemId : GetID(id);
    DZ_ASSERT(popupId != 0 && "BeginPopupContextItem needs an id when the last item has none");
    if (g.mouseReleased[1] && IsItemHovered()) OpenPopupEx(g, popupId);
    return BeginPopupEx(*this, g, popupId, WindowFlags::None);
}

void Ui::EndPopup() {
    UiState& g = *m;
    DZ_ASSERT(!g.beginPopupStack.empty() && "EndPopup() without BeginPopup()");
    End();
    g.beginPopupStack.pop_back();
}

void Ui::CloseCurrentPopup() {
    UiState& g = *m;
    if (!g.beginPopupStack.empty()) ClosePopupToLevel(g, g.beginPopupStack.size() - 1);
}

bool Ui::IsPopupOpen(std::string_view id) const { return IsPopupOpenId(*m, GetID(id)); }

void Ui::BeginTooltip() {
    UiState& g = *m;
    if (!g.nextWindow.hasPos) {
        g.nextWindow.hasPos = true;
        g.nextWindow.pos = g.mousePos + Vec2(g.fontSize, g.fontSize * 0.75f);
        g.nextWindow.posCond = Cond::Always;
        g.nextWindow.pivot = {};
    }
    const uint32_t flags = kWindowTooltip | WindowFlags::NoTitleBar | WindowFlags::NoMove | WindowFlags::NoResize |
                           WindowFlags::NoInputs | WindowFlags::AlwaysAutoResize | WindowFlags::NoScrollbar;
    Begin(DZ_STR("##Tooltip"), nullptr, flags);
}

void Ui::EndTooltip() {
    DZ_ASSERT(m->current && (m->current->flags & kWindowTooltip) && "EndTooltip() without BeginTooltip()");
    End();
}

void Ui::SetTooltip(std::string_view text) {
    BeginTooltip();
    Text(text);
    EndTooltip();
}

// =====================================================================================================================
// Queries
// =====================================================================================================================
bool Ui::IsItemHovered() const {
    const UiState& g = *m;
    const Window* w = g.current;
    if (g.hoveredWindow != w->root || g.disabled > 0) return false;
    if (!(w->lastItemStatus & kItemHoveredRect)) return false;
    return g.activeId == 0 || g.activeId == w->lastItemId;
}

bool Ui::IsItemActive() const { return m->activeId != 0 && m->activeId == m->current->lastItemId; }
bool Ui::IsItemClicked(MouseButton button) const { return m->mouseClicked[int(button)] && IsItemHovered(); }
bool Ui::IsItemEdited() const { return (m->current->lastItemStatus & kItemEdited) != 0; }
bool Ui::IsItemActivated() const { return IsItemActive() && m->activeIdJustActivated; }

bool Ui::IsItemDeactivated() const {
    const ID id = m->current->lastItemId;
    return id != 0 && m->activeIdPrevFrame == id && m->activeId != id;
}

Vec2 Ui::GetItemRectMin() const { return m->current->lastItemRect.min; }
Vec2 Ui::GetItemRectMax() const { return m->current->lastItemRect.max; }
Vec2 Ui::GetItemRectSize() const { return m->current->lastItemRect.Size(); }
bool Ui::IsAnyItemHovered() const { return m->hoveredId != 0 || m->hoveredIdPrev != 0; }
bool Ui::IsAnyItemActive() const { return m->activeId != 0; }
bool Ui::IsMouseHoveringRect(const Rect& rect) const { return rect.Contains(m->mousePos); }
bool Ui::IsKeyDown(Key key) const { return m->input.keysDown[size_t(key)]; }
bool Ui::IsKeyPressed(Key key, bool repeat) const { return IsKeyPressedImpl(*m, key, repeat); }
bool Ui::IsMouseDown(MouseButton button) const { return m->input.mouseDown[int(button)]; }
bool Ui::IsMouseClicked(MouseButton button) const { return m->mouseClicked[int(button)]; }
bool Ui::IsMouseReleased(MouseButton button) const { return m->mouseReleased[int(button)]; }
bool Ui::IsMouseDoubleClicked(MouseButton button) const { return m->mouseDoubleClicked[int(button)]; }
Vec2 Ui::GetMousePos() const { return m->mousePos; }

Vec2 Ui::GetMouseDragDelta(MouseButton button) const {
    const int b = int(button);
    return m->input.mouseDown[b] ? m->mousePos - m->mouseClickedPos[b] : Vec2();
}

void Ui::SetMouseCursor(MouseCursor cursor) { m->cursorRequest = cursor; }

void Ui::SetPrimAllocator(PrimAllocator* allocator) {
    UiState& g = *m;
    DZ_ASSERT(!g.withinFrame && "SetPrimAllocator between frames, not inside one");
    g.primAllocator = allocator;
    g.background.SetPrimAllocator(allocator);
    g.foreground.SetPrimAllocator(allocator);
    for (const auto& w : g.windows) w->drawList.SetPrimAllocator(allocator);
}

// =====================================================================================================================
// ListClipper
// =====================================================================================================================
namespace {

// Puts the layout cursor at the top of row `y` as if every row above it had been submitted.
void SeekRow(UiState& g, float y, float itemHeight) {
    Window* w = g.current;
    w->cursorPos = {std::floor(w->contentStartX + w->indent), y};
    w->cursorMaxPos.y = std::max(w->cursorMaxPos.y, y - g.style.itemSpacing.y);
    w->cursorPosPrevLine.y = y - itemHeight;
    w->prevLineHeight = itemHeight - g.style.itemSpacing.y;
    w->currLineHeight = 0.0f;
    w->currLineTextBaseOffset = 0.0f;
}

} // namespace

void ListClipper::Begin(Ui& ui, int itemCount, float itemHeight) {
    End();
    UiState& g = *ui.m;
    DZ_ASSERT(g.current && "ListClipper::Begin must be inside a window");
    if (TableState* table = CurrentTable(g)) TableEndRow(g, *table);  // rows are measured from a row boundary
    m_ui = &ui;
    m_count = itemCount > 0 ? itemCount : 0;
    m_step = 0;
    m_itemHeight = itemHeight;
    m_startY = g.current->cursorPos.y;
    displayStart = displayEnd = 0;
}

bool ListClipper::Step() {
    if (!m_ui) return false;
    UiState& g = *m_ui->m;
    Window* w = g.current;
    if (m_count == 0 || w->skipItems) {
        End();
        return false;
    }
    TableState* table = CurrentTable(g);
    if (table) TableEndRow(g, *table);  // the rows just submitted end before anything is measured
    int first = 0;  // rows already submitted
    if (m_step == 0 && m_itemHeight <= 0.0f) {
        displayStart = 0;  // submit row 0 to measure the row height
        displayEnd = 1;
        m_step = 1;
        return true;
    }
    if (m_step == 1) {
        first = 1;
        m_itemHeight = w->cursorPos.y - m_startY;
        if (!(m_itemHeight > 0.0f)) {
            // Row 0 took no space (it was a SameLine, or empty): no geometry to clip by, submit everything.
            displayStart = 1;
            displayEnd = m_count;
            m_step = 3;
            if (displayStart < displayEnd) return true;
            End();
            return false;
        }
    }
    if (m_step <= 1) {
        const Rect& clip = w->clipRect;
        int start = int(std::floor((clip.min.y - m_startY) / m_itemHeight));
        int end = int(std::ceil((clip.max.y - m_startY) / m_itemHeight));
        start = std::clamp(start, first, m_count);
        end = std::clamp(end, start, m_count);
        m_step = 2;
        if (start < end) {
            if (start > first) {
                const float y = m_startY + float(start) * m_itemHeight;
                SeekRow(g, y, m_itemHeight);
                if (table) TableSeekRow(*table, y, start);  // keeps row parity (alternating backgrounds)
            }
            displayStart = start;
            displayEnd = end;
            return true;
        }
    }
    End();
    return false;
}

void ListClipper::End() {
    if (!m_ui) return;
    UiState& g = *m_ui->m;
    TableState* table = g.current ? CurrentTable(g) : nullptr;
    if (table) TableEndRow(g, *table);
    // Account for the rows after the last one submitted, so scrolling and the content size see the whole list.
    if (g.current && m_itemHeight > 0.0f && m_count > 0 && !g.current->skipItems) {
        const float endY = m_startY + float(m_count) * m_itemHeight;
        if (g.current->cursorPos.y < endY) {
            SeekRow(g, endY, m_itemHeight);
            if (table) TableSeekRow(*table, endY, m_count);
        }
    }
    m_ui = nullptr;
    displayStart = displayEnd = 0;
}

// =====================================================================================================================
// Settings (window layout persistence)
// =====================================================================================================================
std::string Ui::SaveIniSettings() const {
    UiState& g = *m;
    // Refresh the saved table from every top-level window that exists this run.
    for (const auto& wptr : g.windows) {
        const Window* w = wptr.get();
        if (w->flags & (kWindowChild | kWindowPopup | kWindowTooltip | WindowFlags::NoSavedSettings)) continue;
        if (w->lastFrameActive < 0 || w->name.empty()) continue;
        WindowSettings& s = g.windowSettings[w->id];
        const auto wn = w->name.decode();
        s.name.assign(wn.view().data(), wn.view().size());
        s.pos = w->pos;
        s.size = w->sizeFull;
        s.collapsed = w->collapsed;
        s.hasPos = s.hasSize = true;
    }
    std::string out(DZ_STR("# drizzy UI window layout\n"));
    char line[128];
    for (const auto& entry : g.windowSettings) {
        const WindowSettings& s = entry.second;
        if (s.name.empty()) continue;
        out += DZ_STR("window \"");
        out += s.name;
        out += DZ_STR("\"\n");
        if (s.hasPos) {
            std::snprintf(line, sizeof(line), DZ_STR("pos %d %d\n"), int(s.pos.x), int(s.pos.y));
            out += line;
        }
        if (s.hasSize) {
            std::snprintf(line, sizeof(line), DZ_STR("size %d %d\n"), int(s.size.x), int(s.size.y));
            out += line;
        }
        std::snprintf(line, sizeof(line), DZ_STR("collapsed %d\n\n"), s.collapsed ? 1 : 0);
        out += line;
    }
    return out;
}

void Ui::LoadIniSettings(std::string_view data) {
    UiState& g = *m;
    auto trim = [](std::string_view s) {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return s;
    };
    auto parse2 = [](std::string_view s, float& a, float& b) {
        const std::string tmp(s);
        char* end = nullptr;
        a = std::strtof(tmp.c_str(), &end);
        if (end == tmp.c_str()) return false;
        b = std::strtof(end, &end);
        return true;
    };
    WindowSettings cur;
    bool have = false;
    auto flush = [&] {
        if (have && !cur.name.empty()) g.windowSettings[HashStr(cur.name, 0)] = cur;
        cur = WindowSettings();
        have = false;
    };
    size_t i = 0;
    while (i <= data.size()) {
        const size_t nl = data.find('\n', i);
        std::string_view line = trim(data.substr(i, (nl == std::string_view::npos ? data.size() : nl) - i));
        i = (nl == std::string_view::npos) ? data.size() + 1 : nl + 1;
        if (line.empty() || line.front() == '#') continue;
        if (line.substr(0, 7) == DZ_STR("window ")) {
            flush();
            const size_t q0 = line.find('"');
            const size_t q1 = q0 == std::string_view::npos ? q0 : line.find('"', q0 + 1);
            if (q1 != std::string_view::npos) {
                cur.name.assign(line.substr(q0 + 1, q1 - q0 - 1));
                have = true;
            }
        } else if (line.substr(0, 4) == DZ_STR("pos ")) {
            if (parse2(line.substr(4), cur.pos.x, cur.pos.y)) cur.hasPos = true;
        } else if (line.substr(0, 5) == DZ_STR("size ")) {
            if (parse2(line.substr(5), cur.size.x, cur.size.y)) cur.hasSize = true;
        } else if (line.substr(0, 10) == DZ_STR("collapsed ")) {
            cur.collapsed = std::strtol(std::string(line.substr(10)).c_str(), nullptr, 10) != 0;
        }
    }
    flush();
}

} // namespace drizzy
