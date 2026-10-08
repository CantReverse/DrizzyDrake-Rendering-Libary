// Single-line text input: caret, selection, word navigation, clipboard, filters, password and read-only modes.
#include "ui_internal.h"

#include "../text_layout.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace drizzy {
using namespace ui_detail;

namespace {

bool IsWordChar(uint32_t c) {
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

int PrevBoundary(const std::string& s, int i) {
    if (i <= 0) return 0;
    --i;
    while (i > 0 && (uint8_t(s[size_t(i)]) & 0xC0u) == 0x80u) --i;
    return i;
}

int NextBoundary(const std::string& s, int i) {
    const int n = int(s.size());
    if (i >= n) return n;
    ++i;
    while (i < n && (uint8_t(s[size_t(i)]) & 0xC0u) == 0x80u) ++i;
    return i;
}

uint32_t CodepointAt(const std::string& s, int i) {
    if (i >= int(s.size())) return 0;
    const char* p = s.data() + i;
    return detail::DecodeUtf8(p, s.data() + s.size());
}

int WordLeft(const std::string& s, int i) {
    while (i > 0 && !IsWordChar(CodepointAt(s, PrevBoundary(s, i)))) i = PrevBoundary(s, i);
    while (i > 0 && IsWordChar(CodepointAt(s, PrevBoundary(s, i)))) i = PrevBoundary(s, i);
    return i;
}

int WordRight(const std::string& s, int i) {
    const int n = int(s.size());
    while (i < n && IsWordChar(CodepointAt(s, i))) i = NextBoundary(s, i);
    while (i < n && !IsWordChar(CodepointAt(s, i))) i = NextBoundary(s, i);
    return i;
}

void AppendUtf8(std::string& out, uint32_t c) {
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

// Applies the character filters; returns false to drop the character.
bool FilterCharacter(uint32_t& c, uint32_t flags) {
    if (c < 0x20 || c == 0x7F || (c >= 0xD800 && c <= 0xDFFF) || c > 0x10FFFF) return false;
    if ((flags & InputTextFlags::CharsDecimal) &&
        !((c >= '0' && c <= '9') || c == '.' || c == '+' || c == '-' || c == 'e' || c == 'E')) {
        return false;
    }
    if ((flags & InputTextFlags::CharsNoBlank) && (c == ' ' || c == '\t')) return false;
    if ((flags & InputTextFlags::CharsUppercase) && c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return true;
}

// Caret x for every code point boundary of the edit buffer, computed exactly as AddText advances the pen.
void LayoutCarets(UiState& g, InputTextState& s, bool password) {
    s.boundaries.clear();
    s.offsets.clear();
    const char* begin = s.text.data();
    const char* end = begin + s.text.size();
    float pen = 0.0f;
    uint32_t prev = 0;
    for (const char* p = begin; p < end;) {
        s.boundaries.push_back(int(p - begin));
        s.offsets.push_back(pen);
        uint32_t c = detail::DecodeUtf8(p, end);
        if (password) c = '*';
        pen += detail::AdvanceEm(*g.font, prev, c) * g.fontSize;
        prev = c;
    }
    s.boundaries.push_back(int(s.text.size()));
    s.offsets.push_back(pen);
}

float CaretX(const InputTextState& s, int byteIndex) {
    const auto it = std::lower_bound(s.boundaries.begin(), s.boundaries.end(), byteIndex);
    if (it == s.boundaries.end()) return s.offsets.back();
    return s.offsets[size_t(it - s.boundaries.begin())];
}

int ByteIndexAtX(const InputTextState& s, float x) {
    size_t best = 0;
    for (size_t i = 1; i < s.offsets.size(); ++i) {
        if (std::fabs(s.offsets[i] - x) < std::fabs(s.offsets[best] - x)) best = i;
        if (s.offsets[i] > x) break;
    }
    return s.boundaries[best];
}

// The text as displayed: '*' per code point in password mode.
std::string_view DisplayText(UiState& g, std::string_view text, bool password) {
    if (!password) return text;
    g.scratchText.clear();
    const char* end = text.data() + text.size();
    for (const char* p = text.data(); p < end;) {
        detail::DecodeUtf8(p, end);
        g.scratchText += '*';
    }
    return g.scratchText;
}

bool InputTextImpl(UiState& g, std::string_view label, std::string_view hint, char* buffer, size_t bufferSize,
                   std::string* target, uint32_t flags) {
    Window* w = g.current;
    if (w->skipItems) return false;
    DZ_ASSERT(buffer ? bufferSize > 0 : target != nullptr);
    const auto [id, labelText] = ParseLabel(label, w->idStack.back());
    const Vec2 pad = g.style.framePadding;
    const float lineH = TextLineHeight(g);
    const Rect frame = Rect::FromPosSize(w->cursorPos, {CalcItemWidth(g), lineH + pad.y * 2.0f});
    const float labelW = TextSize(g, labelText).x;
    const Rect total(frame.min, {frame.max.x + (labelW > 0.0f ? g.style.itemInnerSpacing.x + labelW : 0.0f), frame.max.y});
    ItemSize(g, total.Size(), pad.y);
    if (!ItemAdd(g, total, id)) return false;

    const bool hovered = ItemHoverable(g, frame, id);
    if (hovered) g.cursorRequest = MouseCursor::TextInput;
    const bool readOnly = (flags & InputTextFlags::ReadOnly) != 0;
    const bool password = (flags & InputTextFlags::Password) != 0;
    const std::string_view current = buffer ? std::string_view(buffer) : std::string_view(*target);
    const size_t maxBytes = buffer ? bufferSize - 1 : SIZE_MAX;
    InputTextState& s = g.inputText;
    const float innerW = std::max(frame.Width() - pad.x * 2.0f, 1.0f);

    bool justActivated = false;
    if (hovered && g.mouseClicked[0] && g.activeId != id) {
        SetActiveID(g, id, w);
        justActivated = true;
        s.id = id;
        s.text.assign(current);
        s.initialText = s.text;
        s.scrollX = 0.0f;
        s.blinkTime = 0.0f;
        LayoutCarets(g, s, password);
        if (flags & InputTextFlags::AutoSelectAll) {
            s.selStart = 0;
            s.cursor = int(s.text.size());
        } else {
            s.cursor = s.selStart = ByteIndexAtX(s, g.mousePos.x - (frame.min.x + pad.x));
        }
    }

    bool edited = false, enterPressed = false, deactivate = false;
    const bool active = g.activeId == id && s.id == id;
    if (active) {
        const bool ctrl = g.input.keyCtrl, shift = g.input.keyShift, alt = g.input.keyAlt;
        const float originX = frame.min.x + pad.x - s.scrollX;
        auto hasSelection = [&] { return s.selStart != s.cursor; };
        auto selMin = [&] { return std::min(s.selStart, s.cursor); };
        auto selMax = [&] { return std::max(s.selStart, s.cursor); };
        auto moveTo = [&](int index) {
            s.cursor = index;
            if (!shift) s.selStart = index;
            s.blinkTime = 0.0f;
        };
        auto eraseRange = [&](int from, int to) {
            if (from >= to) return;
            s.text.erase(size_t(from), size_t(to - from));
            s.cursor = s.selStart = from;
            edited = true;
        };
        auto insertText = [&](std::string_view text) {
            if (readOnly) return;
            eraseRange(selMin(), selMax());
            if (s.text.size() + text.size() > maxBytes) return;
            s.text.insert(size_t(s.cursor), text);
            s.cursor = s.selStart = s.cursor + int(text.size());
            edited = true;
        };

        if (g.mouseClicked[0] && !hovered) {
            deactivate = true;  // clicked elsewhere
        } else if (!justActivated && g.mouseClicked[0]) {
            const int index = ByteIndexAtX(s, g.mousePos.x - originX);
            if (g.mouseDoubleClicked[0]) {
                s.selStart = WordLeft(s.text, NextBoundary(s.text, index));
                s.cursor = WordRight(s.text, s.selStart);
                while (s.cursor > s.selStart && !IsWordChar(CodepointAt(s.text, PrevBoundary(s.text, s.cursor)))) {
                    s.cursor = PrevBoundary(s.text, s.cursor);
                }
            } else {
                moveTo(index);
            }
        } else if (g.input.mouseDown[0] && !justActivated && !g.mouseDoubleClicked[0] && g.mouseDelta.x != 0.0f) {
            s.cursor = ByteIndexAtX(s, g.mousePos.x - originX);  // drag-select
        }

        const int end = int(s.text.size());
        if (IsKeyPressedImpl(g, Key::LeftArrow, true)) {
            moveTo(hasSelection() && !shift ? selMin() : ctrl ? WordLeft(s.text, s.cursor) : PrevBoundary(s.text, s.cursor));
        } else if (IsKeyPressedImpl(g, Key::RightArrow, true)) {
            moveTo(hasSelection() && !shift ? selMax() : ctrl ? WordRight(s.text, s.cursor) : NextBoundary(s.text, s.cursor));
        } else if (IsKeyPressedImpl(g, Key::Home, true) || IsKeyPressedImpl(g, Key::UpArrow, true)) {
            moveTo(0);
        } else if (IsKeyPressedImpl(g, Key::End, true) || IsKeyPressedImpl(g, Key::DownArrow, true)) {
            moveTo(end);
        } else if (IsKeyPressedImpl(g, Key::Backspace, true) && !readOnly) {
            if (hasSelection()) eraseRange(selMin(), selMax());
            else eraseRange(ctrl ? WordLeft(s.text, s.cursor) : PrevBoundary(s.text, s.cursor), s.cursor);
        } else if (IsKeyPressedImpl(g, Key::Delete, true) && !readOnly) {
            if (hasSelection()) eraseRange(selMin(), selMax());
            else eraseRange(s.cursor, ctrl ? WordRight(s.text, s.cursor) : NextBoundary(s.text, s.cursor));
        } else if (ctrl && IsKeyPressedImpl(g, Key::A, false)) {
            s.selStart = 0;
            s.cursor = end;
        } else if (ctrl && (IsKeyPressedImpl(g, Key::C, false) || IsKeyPressedImpl(g, Key::X, false))) {
            const bool cut = IsKeyPressedImpl(g, Key::X, false) && !readOnly;
            if (hasSelection() && !password && g.platform.setClipboardText) {
                const std::string copy = s.text.substr(size_t(selMin()), size_t(selMax() - selMin()));
                g.platform.setClipboardText(g.platform.userData, copy.c_str());
            }
            if (cut && hasSelection() && !password) eraseRange(selMin(), selMax());
        } else if (ctrl && IsKeyPressedImpl(g, Key::V, true) && !readOnly && g.platform.getClipboardText) {
            if (const char* clip = g.platform.getClipboardText(g.platform.userData)) {
                std::string filtered;
                const std::string_view clipText(clip);
                const char* clipEnd = clipText.data() + clipText.size();
                for (const char* p = clipText.data(); p < clipEnd;) {
                    uint32_t c = detail::DecodeUtf8(p, clipEnd);
                    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
                    if (FilterCharacter(c, flags)) AppendUtf8(filtered, c);
                }
                // Keep whole code points when the buffer cannot take everything.
                if (s.text.size() - size_t(selMax() - selMin()) + filtered.size() > maxBytes) {
                    size_t room = maxBytes - (s.text.size() - size_t(selMax() - selMin()));
                    while (room > 0 && room < filtered.size() && (uint8_t(filtered[room]) & 0xC0u) == 0x80u) --room;
                    filtered.resize(std::min(room, filtered.size()));
                }
                insertText(filtered);
            }
        } else if (IsKeyPressedImpl(g, Key::Enter, false)) {
            enterPressed = true;
            deactivate = true;
        } else if (IsKeyPressedImpl(g, Key::Escape, false)) {
            if (s.text != s.initialText) {
                s.text = s.initialText;
                edited = true;
            }
            deactivate = true;
        } else if (IsKeyPressedImpl(g, Key::Tab, false)) {
            deactivate = true;
        }

        if (!(ctrl && !alt) && !readOnly) {  // AltGr (Ctrl+Alt) still types characters
            std::string utf8;
            for (uint32_t c : g.input.characters) {
                if (FilterCharacter(c, flags)) {
                    utf8.clear();
                    AppendUtf8(utf8, c);
                    insertText(utf8);
                }
            }
        }
        if (!g.input.characters.empty()) s.blinkTime = 0.0f;

        if (edited) {
            LayoutCarets(g, s, password);
            if (buffer) {
                std::memcpy(buffer, s.text.data(), s.text.size());
                buffer[s.text.size()] = '\0';
            } else {
                *target = s.text;
            }
            w->lastItemStatus |= kItemEdited;
        }
        if (deactivate) {
            ClearActiveID(g);
            s.id = 0;
        }
    }

    // ---- Render
    DrawList& dl = *w->dl;
    const bool showActive = active && !deactivate;
    RenderFieldFrame(g, frame, hovered, showActive);
    const float textY = frame.min.y + pad.y;
    const Rect clip = Rect(frame.min.x + 1.0f, frame.min.y, frame.max.x - 1.0f, frame.max.y).Intersect(w->clipRect);
    if (showActive) {
        const float caretX = CaretX(s, s.cursor);
        const float textW = s.offsets.back();
        if (caretX - s.scrollX > innerW) s.scrollX = caretX - innerW;
        else if (caretX < s.scrollX) s.scrollX = std::max(0.0f, caretX - innerW * 0.25f);
        s.scrollX = std::max(0.0f, std::min(s.scrollX, std::max(0.0f, textW - innerW + 1.0f)));
        const float originX = frame.min.x + pad.x - s.scrollX;
        // Scrolled or overflowing text is clipped on the CPU (glyphs trimmed, rects intersected): no extra draw command.
        const bool needClip = s.scrollX > 0.0f || textW > innerW;
        auto clippedRect = [&](Rect r, Color color) {
            if (needClip) r = r.Intersect(clip);
            if (r.Width() > 0.0f && r.Height() > 0.0f) dl.AddRectFilled(r, color);
        };
        if (s.selStart != s.cursor) {
            const float x0 = originX + CaretX(s, std::min(s.selStart, s.cursor));
            const float x1 = originX + CaretX(s, std::max(s.selStart, s.cursor));
            clippedRect(Rect(x0, textY, x1, textY + lineH), StyleColor(g, UiColor::TextSelectedBg));
        }
        const std::string_view shownText = DisplayText(g, s.text, password);
        if (needClip) {
            RenderTextClipped(g, {originX, textY}, shownText, StyleColor(g, UiColor::Text), clip);
        } else {
            RenderText(g, {originX, textY}, shownText, StyleColor(g, UiColor::Text));
        }
        s.blinkTime += g.input.deltaTime;
        if (std::fmod(s.blinkTime, 1.2f) < 0.8f) {
            const float cx = std::floor(originX + caretX);
            clippedRect(Rect(cx, textY, cx + 1.0f, textY + lineH), StyleColor(g, UiColor::Text));
        }
        g.output.textInputPos = {originX + caretX, textY + lineH};
    } else {
        const std::string_view shown = current.empty() ? hint : DisplayText(g, current, password);
        const Color color = StyleColor(g, current.empty() ? UiColor::TextDisabled : UiColor::Text);
        if (TextSize(g, shown).x > innerW) {
            RenderTextClipped(g, {frame.min.x + pad.x, textY}, shown, color, clip);
        } else {
            RenderText(g, {frame.min.x + pad.x, textY}, shown, color);
        }
    }
    if (!labelText.empty()) {
        RenderText(g, {frame.max.x + g.style.itemInnerSpacing.x, textY}, labelText, StyleColor(g, UiColor::Text));
    }
    return (flags & InputTextFlags::EnterReturnsTrue) ? enterPressed : edited;
}

// ---------------------------------------------------------------------------------------------------------------------
// Multi-line editor
// ---------------------------------------------------------------------------------------------------------------------
int LineStartOf(const std::string& s, int i) {
    while (i > 0 && s[size_t(i) - 1] != '\n') --i;
    return i;
}
int LineEndOf(const std::string& s, int i) {
    const int n = int(s.size());
    while (i < n && s[size_t(i)] != '\n') ++i;
    return i;
}
int LineIndexOf(const std::string& s, int byte) {
    int line = 0;
    for (int i = 0; i < byte; ++i) {
        if (s[size_t(i)] == '\n') ++line;
    }
    return line;
}
int LineStartByIndex(const std::string& s, int line) {
    int i = 0, ln = 0;
    const int n = int(s.size());
    while (ln < line && i < n) {
        if (s[size_t(i)] == '\n') ++ln;
        ++i;
    }
    return i;
}
int LineCountOf(const std::string& s) {
    int c = 1;
    for (char ch : s) {
        if (ch == '\n') ++c;
    }
    return c;
}
float ColumnX(const Font& font, float size, const std::string& s, int lineStart, int byte) {
    float pen = 0.0f;
    uint32_t prev = 0;
    const char* p = s.data() + lineStart;
    const char* end = s.data() + byte;
    while (p < end) {
        const uint32_t c = detail::DecodeUtf8(p, s.data() + s.size());
        pen += detail::AdvanceEm(font, prev, c) * size;
        prev = c;
    }
    return pen;
}
int ByteAtColumn(const Font& font, float size, const std::string& s, int lineStart, int lineEnd, float x) {
    const char* base = s.data();
    const char* p = base + lineStart;
    const char* end = base + lineEnd;
    float pen = 0.0f;
    uint32_t prev = 0;
    int best = lineStart;
    float bestDist = std::fabs(x);
    while (p < end) {
        const uint32_t c = detail::DecodeUtf8(p, base + s.size());
        pen += detail::AdvanceEm(font, prev, c) * size;
        prev = c;
        if (std::fabs(pen - x) < bestDist) {
            bestDist = std::fabs(pen - x);
            best = int(p - base);
        }
    }
    return best;
}

bool InputTextMultilineImpl(UiState& g, std::string_view label, char* buffer, size_t bufferSize, std::string* target,
                            Vec2 sizeArg, uint32_t flags) {
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, labelText] = ParseLabel(label, w->idStack.back());
    const Vec2 pad = g.style.framePadding;
    const float lineH = TextLineHeight(g);
    const Vec2 size = CalcItemSize(g, sizeArg, ui_detail::CalcItemWidth(g), lineH * 6.0f + pad.y * 2.0f);
    const Rect frame = Rect::FromPosSize(w->cursorPos, size);
    const float labelW = TextSize(g, labelText).x;
    const Rect total(frame.min, {frame.max.x + (labelW > 0.0f ? g.style.itemInnerSpacing.x + labelW : 0.0f), frame.max.y});
    ItemSize(g, total.Size(), pad.y);
    if (!ItemAdd(g, total, id)) return false;

    const bool hovered = ItemHoverable(g, frame, id);
    if (hovered) g.cursorRequest = MouseCursor::TextInput;
    const bool readOnly = (flags & InputTextFlags::ReadOnly) != 0;
    const std::string_view current = buffer ? std::string_view(buffer) : std::string_view(*target);
    const size_t maxBytes = buffer ? bufferSize - 1 : SIZE_MAX;
    InputTextState& s = g.inputText;
    const Rect inner(frame.min + pad, frame.max - pad);
    const float viewH = inner.Height();

    auto mouseToByte = [&](Vec2 mouse) {
        const int line = std::max(0, int(std::floor((mouse.y - inner.min.y + s.scrollY) / lineH)));
        const int clampedLine = std::min(line, LineCountOf(s.text) - 1);
        const int ls = LineStartByIndex(s.text, clampedLine);
        const int le = LineEndOf(s.text, ls);
        return ByteAtColumn(*g.font, g.fontSize, s.text, ls, le, mouse.x - inner.min.x + s.scrollX);
    };

    bool justActivated = false;
    if (hovered && g.mouseClicked[0] && g.activeId != id) {
        SetActiveID(g, id, w);
        justActivated = true;
        s.id = id;
        s.text.assign(current);
        s.initialText = s.text;
        s.scrollX = s.scrollY = 0.0f;
        s.blinkTime = 0.0f;
        s.preferredX = -1.0f;
        s.cursor = s.selStart = mouseToByte(g.mousePos);
    }

    bool edited = false, deactivate = false;
    const bool active = g.activeId == id && s.id == id;
    if (active) {
        const bool ctrl = g.input.keyCtrl, shift = g.input.keyShift, alt = g.input.keyAlt;
        auto selMin = [&] { return std::min(s.selStart, s.cursor); };
        auto selMax = [&] { return std::max(s.selStart, s.cursor); };
        auto hasSel = [&] { return s.selStart != s.cursor; };
        auto moveTo = [&](int index, bool vertical = false) {
            s.cursor = index;
            if (!shift) s.selStart = index;
            s.blinkTime = 0.0f;
            if (!vertical) s.preferredX = -1.0f;
        };
        auto eraseRange = [&](int from, int to) {
            if (from >= to) return;
            s.text.erase(size_t(from), size_t(to - from));
            s.cursor = s.selStart = from;
            edited = true;
            s.preferredX = -1.0f;
        };
        auto insertText = [&](std::string_view text) {
            if (readOnly) return;
            eraseRange(selMin(), selMax());
            if (s.text.size() + text.size() > maxBytes) return;
            s.text.insert(size_t(s.cursor), text);
            s.cursor = s.selStart = s.cursor + int(text.size());
            edited = true;
        };
        auto verticalMove = [&](int delta) {
            const int line = LineIndexOf(s.text, s.cursor);
            const int ls = LineStartOf(s.text, s.cursor);
            if (s.preferredX < 0.0f) s.preferredX = ColumnX(*g.font, g.fontSize, s.text, ls, s.cursor);
            const int target = int(Clamp(float(line + delta), 0.0f, float(LineCountOf(s.text) - 1)));
            const int tls = LineStartByIndex(s.text, target);
            const int tle = LineEndOf(s.text, tls);
            moveTo(ByteAtColumn(*g.font, g.fontSize, s.text, tls, tle, s.preferredX), true);
        };

        if (g.mouseClicked[0] && !hovered) {
            deactivate = true;
        } else if (!justActivated && g.input.mouseDown[0] && g.mouseDelta.x == 0.0f && g.mouseDelta.y == 0.0f &&
                   g.mouseClicked[0]) {
            moveTo(mouseToByte(g.mousePos));
        } else if (!justActivated && g.input.mouseDown[0] && (g.mouseDelta.x != 0.0f || g.mouseDelta.y != 0.0f)) {
            s.cursor = mouseToByte(g.mousePos);  // drag-select
        }

        const int end = int(s.text.size());
        if (IsKeyPressedImpl(g, Key::LeftArrow, true)) {
            moveTo(hasSel() && !shift ? selMin() : ctrl ? WordLeft(s.text, s.cursor) : PrevBoundary(s.text, s.cursor));
        } else if (IsKeyPressedImpl(g, Key::RightArrow, true)) {
            moveTo(hasSel() && !shift ? selMax() : ctrl ? WordRight(s.text, s.cursor) : NextBoundary(s.text, s.cursor));
        } else if (IsKeyPressedImpl(g, Key::UpArrow, true)) {
            verticalMove(-1);
        } else if (IsKeyPressedImpl(g, Key::DownArrow, true)) {
            verticalMove(1);
        } else if (IsKeyPressedImpl(g, Key::Home, true)) {
            moveTo(LineStartOf(s.text, s.cursor));
        } else if (IsKeyPressedImpl(g, Key::End, true)) {
            moveTo(LineEndOf(s.text, s.cursor));
        } else if (IsKeyPressedImpl(g, Key::Backspace, true) && !readOnly) {
            if (hasSel()) eraseRange(selMin(), selMax());
            else eraseRange(ctrl ? WordLeft(s.text, s.cursor) : PrevBoundary(s.text, s.cursor), s.cursor);
        } else if (IsKeyPressedImpl(g, Key::Delete, true) && !readOnly) {
            if (hasSel()) eraseRange(selMin(), selMax());
            else eraseRange(s.cursor, ctrl ? WordRight(s.text, s.cursor) : NextBoundary(s.text, s.cursor));
        } else if (ctrl && IsKeyPressedImpl(g, Key::A, false)) {
            s.selStart = 0;
            s.cursor = end;
        } else if (ctrl && (IsKeyPressedImpl(g, Key::C, false) || IsKeyPressedImpl(g, Key::X, false))) {
            if (hasSel() && g.platform.setClipboardText) {
                const std::string copy = s.text.substr(size_t(selMin()), size_t(selMax() - selMin()));
                g.platform.setClipboardText(g.platform.userData, copy.c_str());
            }
            if (IsKeyPressedImpl(g, Key::X, false) && hasSel() && !readOnly) eraseRange(selMin(), selMax());
        } else if (ctrl && IsKeyPressedImpl(g, Key::V, true) && !readOnly && g.platform.getClipboardText) {
            if (const char* clip = g.platform.getClipboardText(g.platform.userData)) insertText(clip);
        } else if (IsKeyPressedImpl(g, Key::Enter, true) && !readOnly) {
            insertText("\n");
        } else if (IsKeyPressedImpl(g, Key::Escape, false)) {
            if (s.text != s.initialText) {
                s.text = s.initialText;
                edited = true;
            }
            deactivate = true;
        }

        if (!(ctrl && !alt) && !readOnly) {
            std::string utf8;
            for (uint32_t c : g.input.characters) {
                if (FilterCharacter(c, flags)) {
                    utf8.clear();
                    AppendUtf8(utf8, c);
                    insertText(utf8);
                }
            }
        }
        if (!g.input.characters.empty()) s.blinkTime = 0.0f;

        if (hovered && g.input.mouseWheel != 0.0f) s.scrollY -= g.input.mouseWheel * lineH * 3.0f;

        if (edited) {
            if (buffer) {
                std::memcpy(buffer, s.text.data(), s.text.size());
                buffer[s.text.size()] = '\0';
            } else {
                *target = s.text;
            }
            w->lastItemStatus |= kItemEdited;
        }
        if (deactivate) {
            ClearActiveID(g);
            s.id = 0;
        }
    }

    // ---- Render
    DrawList& dl = *w->dl;
    const bool showActive = active && !deactivate;
    RenderFieldFrame(g, frame, hovered, showActive);
    const std::string& shown = showActive ? s.text : std::string(current);
    const int lineCount = LineCountOf(shown);

    if (showActive) {
        // Keep the caret visible.
        const int caretLine = LineIndexOf(s.text, s.cursor);
        const float caretX = ColumnX(*g.font, g.fontSize, s.text, LineStartOf(s.text, s.cursor), s.cursor);
        const float caretTop = float(caretLine) * lineH;
        s.scrollY = Clamp(s.scrollY, std::max(0.0f, caretTop + lineH - viewH), caretTop);
        const float maxScrollY = std::max(0.0f, float(lineCount) * lineH - viewH);
        s.scrollY = Clamp(s.scrollY, 0.0f, maxScrollY);
        if (caretX - s.scrollX > inner.Width()) s.scrollX = caretX - inner.Width();
        else if (caretX < s.scrollX) s.scrollX = std::max(0.0f, caretX);
    }
    dl.PushClipRect(inner.Intersect(w->clipRect));
    const int firstLine = std::max(0, int((showActive ? s.scrollY : 0.0f) / lineH));
    const int lastLine = std::min(lineCount, firstLine + int(viewH / lineH) + 2);
    const float originX = inner.min.x - (showActive ? s.scrollX : 0.0f);
    const float originY = inner.min.y - (showActive ? s.scrollY : 0.0f);
    const std::string& textRef = showActive ? s.text : shown;
    const int selLo = showActive ? std::min(s.selStart, s.cursor) : 0;
    const int selHi = showActive ? std::max(s.selStart, s.cursor) : 0;
    for (int line = firstLine; line < lastLine; ++line) {
        const int ls = LineStartByIndex(textRef, line);
        const int le = LineEndOf(textRef, ls);
        const float y = originY + float(line) * lineH;
        if (showActive && selLo != selHi && selHi > ls && selLo <= le) {
            const int a = std::max(selLo, ls), b = std::min(selHi, le);
            const float x0 = originX + ColumnX(*g.font, g.fontSize, textRef, ls, a);
            const float x1 = originX + ColumnX(*g.font, g.fontSize, textRef, ls, b) + (selHi > le ? g.fontSize * 0.3f : 0.0f);
            dl.AddRectFilled(Rect(x0, y, x1, y + lineH), StyleColor(g, UiColor::TextSelectedBg));
        }
        if (le > ls) {
            RenderText(g, {originX, y}, std::string_view(textRef.data() + ls, size_t(le - ls)),
                       StyleColor(g, current.empty() && !showActive ? UiColor::TextDisabled : UiColor::Text));
        }
    }
    if (showActive) {
        s.blinkTime += g.input.deltaTime;
        if (std::fmod(s.blinkTime, 1.2f) < 0.8f) {
            const int caretLine = LineIndexOf(s.text, s.cursor);
            const float cx = std::floor(originX + ColumnX(*g.font, g.fontSize, s.text, LineStartOf(s.text, s.cursor), s.cursor));
            const float cy = originY + float(caretLine) * lineH;
            dl.AddRectFilled(Rect(cx, cy, cx + 1.0f, cy + lineH), StyleColor(g, UiColor::Text));
        }
        g.output.textInputPos = inner.min;
    }
    dl.PopClipRect();
    if (!labelText.empty()) {
        RenderText(g, {frame.max.x + g.style.itemInnerSpacing.x, frame.min.y + pad.y}, labelText,
                   StyleColor(g, UiColor::Text));
    }
    return edited;
}

} // namespace

bool Ui::InputTextMultiline(std::string_view label, char* buffer, size_t bufferSize, Vec2 size, uint32_t flags) {
    return InputTextMultilineImpl(*m, label, buffer, bufferSize, nullptr, size, flags | InputTextFlags::Multiline);
}

bool Ui::InputTextMultiline(std::string_view label, std::string* text, Vec2 size, uint32_t flags) {
    return InputTextMultilineImpl(*m, label, nullptr, 0, text, size, flags | InputTextFlags::Multiline);
}

bool Ui::InputText(std::string_view label, char* buffer, size_t bufferSize, uint32_t flags) {
    return InputTextImpl(*m, label, {}, buffer, bufferSize, nullptr, flags);
}

bool Ui::InputText(std::string_view label, std::string* text, uint32_t flags) {
    return InputTextImpl(*m, label, {}, nullptr, 0, text, flags);
}

bool Ui::InputTextWithHint(std::string_view label, std::string_view hint, char* buffer, size_t bufferSize,
                           uint32_t flags) {
    return InputTextImpl(*m, label, hint, buffer, bufferSize, nullptr, flags);
}

bool Ui::InputInt(std::string_view label, int* value, uint32_t flags) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%d", *value);
    const bool changed = InputText(label, buffer, sizeof(buffer), flags | InputTextFlags::CharsDecimal);
    if (changed) {
        char* end = nullptr;
        const long parsed = std::strtol(buffer, &end, 10);
        if (end != buffer) *value = int(parsed);
    }
    return changed;
}

bool Ui::InputFloat(std::string_view label, float* value, const char* format, uint32_t flags) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format, double(*value));
    const bool changed = InputText(label, buffer, sizeof(buffer), flags | InputTextFlags::CharsDecimal);
    if (changed) {
        char* end = nullptr;
        const float parsed = std::strtof(buffer, &end);
        if (end != buffer) *value = parsed;
    }
    return changed;
}

} // namespace drizzy
