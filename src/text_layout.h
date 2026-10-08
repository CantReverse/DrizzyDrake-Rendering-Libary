// Internal: UTF-8 decoding and line breaking shared by Font::MeasureText and DrawList::AddText, so measuring and
// drawing always agree.
#pragma once

#include "drizzy/font.h"

namespace drizzy::detail {

// Decodes one code point and advances `s`. Malformed sequences decode to U+FFFD.
DZ_FORCEINLINE uint32_t DecodeUtf8(const char*& s, const char* end) {
    const uint32_t c = uint8_t(*s++);
    if (c < 0x80) return c;
    uint32_t cp;
    int extra;
    if (c >= 0xF8) return 0xFFFD;
    if (c >= 0xF0) {
        cp = c & 0x07u;
        extra = 3;
    } else if (c >= 0xE0) {
        cp = c & 0x0Fu;
        extra = 2;
    } else if (c >= 0xC0) {
        cp = c & 0x1Fu;
        extra = 1;
    } else {
        return 0xFFFD;  // stray continuation byte
    }
    for (int i = 0; i < extra; ++i) {
        if (s >= end || (uint8_t(*s) & 0xC0u) != 0x80u) return 0xFFFD;
        cp = (cp << 6) | (uint8_t(*s++) & 0x3Fu);
    }
    return cp;
}

// Two texture coordinates as unorm16, x in the low half: how glyphs store theirs (Glyph::uvPacked, GpuPrim data).
DZ_FORCEINLINE uint32_t PackUnorm16x2(float x, float y) {
    auto q = [](float v) { return uint32_t(Clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); };
    return q(x) | (q(y) << 16);
}

// Pen advance from `prev` (0 at the start of a line) to and across `c`, in em.
DZ_FORCEINLINE float AdvanceEm(const Font& font, uint32_t prev, uint32_t c) { return font.Advance(prev, c); }

struct LineInfo {
    const char* end;   // end of the line's drawn content
    const char* next;  // start of the following line
    float width;       // pixels; computed when wrapping or when requested
    bool newline;      // the line was ended by '\n'
};

// Finds the line starting at `s`. Without wrapping, lines end at '\n'. With wrapping, a line also ends at the last
// space before the text would exceed wrapWidth; the spaces at that break are dropped, and a single word wider than
// wrapWidth is split. Every line consumes at least one character, so callers always make progress.
LineInfo NextLine(const Font& font, const char* s, const char* end, float size, float wrapWidth, bool needWidth);

} // namespace drizzy::detail
