#include "text_layout.h"

#include <algorithm>
#include <cstring>

namespace drizzy {
namespace detail {

// Kept out of line: inlined into the glyph loops by link-time code generation, they compiled measurably slower.
DZ_NOINLINE LineInfo NextLine(const Font& font, const char* s, const char* end, float size, float wrapWidth,
                              bool needWidth) {
    if (wrapWidth <= 0.0f) {
        const char* nl = static_cast<const char*>(std::memchr(s, '\n', size_t(end - s)));
        LineInfo line = {nl ? nl : end, nl ? nl + 1 : end, 0.0f, nl != nullptr};
        if (needWidth) {
            float pen = 0.0f;
            uint32_t prev = 0;
            for (const char* p = s; p < line.end;) {
                const uint32_t c = DecodeUtf8(p, line.end);
                pen += AdvanceEm(font, prev, c);
                prev = c;
            }
            line.width = pen * size;
        }
        return line;
    }

    float pen = 0.0f;             // pen position in pixels, including trailing spaces
    float contentWidth = 0.0f;    // pen position after the last non-space character
    const char* contentEnd = s;   // end of the last non-space character
    bool inSpace = false;
    // The most recent place the line may break: content before a run of spaces, and the start of the next word.
    const char* breakEnd = nullptr;
    const char* breakNext = nullptr;
    float breakWidth = 0.0f;
    uint32_t prev = 0;
    for (const char* p = s; p < end;) {
        const char* charStart = p;
        const uint32_t c = DecodeUtf8(p, end);
        if (c == '\n') return {contentEnd, p, contentWidth, true};
        const float advance = AdvanceEm(font, prev, c) * size;
        prev = c;
        if (c == ' ' || c == '\t') {
            if (!inSpace) {
                breakEnd = contentEnd;
                breakWidth = contentWidth;
                inSpace = true;
            }
            pen += advance;
            continue;
        }
        if (inSpace) {
            breakNext = charStart;
            inSpace = false;
        }
        if (pen + advance > wrapWidth && contentEnd != s) {
            if (breakEnd && breakEnd != s) return {breakEnd, breakNext, breakWidth, false};
            return {contentEnd, charStart, contentWidth, false};  // one word wider than the line: split it
        }
        pen += advance;
        contentEnd = p;
        contentWidth = pen;
    }
    return {contentEnd, end, contentWidth, false};
}

} // namespace detail

const Glyph& Font::GetGlyphSlow(uint32_t codepoint) const {
    const auto it = std::lower_bound(m_glyphs.begin(), m_glyphs.end(), codepoint,
                                     [](const Glyph& g, uint32_t cp) { return g.codepoint < cp; });
    return (it != m_glyphs.end() && it->codepoint == codepoint) ? *it : m_glyphs[m_fallback];
}

bool Font::HasGlyph(uint32_t codepoint) const {
    const auto it = std::lower_bound(m_glyphs.begin(), m_glyphs.end(), codepoint,
                                     [](const Glyph& g, uint32_t cp) { return g.codepoint < cp; });
    return it != m_glyphs.end() && it->codepoint == codepoint;
}

float Font::KerningSlow(uint32_t left, uint32_t right) const {
    const uint64_t key = (uint64_t(left) << 32) | right;
    const auto it = std::lower_bound(m_kerningPairs.begin(), m_kerningPairs.end(), key,
                                     [](const std::pair<uint64_t, float>& p, uint64_t k) { return p.first < k; });
    return (it != m_kerningPairs.end() && it->first == key) ? it->second : 0.0f;
}

Vec2 Font::MeasureText(std::string_view text, float size, float wrapWidth, float lineSpacing) const {
    if (text.empty() || !(size > 0.0f)) return {};
    const char* s = text.data();
    const char* end = s + text.size();
    float width = 0.0f;
    uint32_t lines = 0;
    for (;;) {
        const detail::LineInfo line = detail::NextLine(*this, s, end, size, wrapWidth, true);
        width = std::max(width, line.width);
        ++lines;
        if (line.next >= end && !line.newline) break;
        s = line.next;
    }
    return {width, float(lines) * m_lineHeight * size * lineSpacing};
}

} // namespace drizzy
