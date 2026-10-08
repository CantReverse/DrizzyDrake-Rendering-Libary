// drizzy_renderer - fonts.
//
// A FontAtlas turns TrueType / OpenType fonts into one RGBA8 texture holding a multi-channel signed distance field
// (MTSDF) per glyph: RGB keep corners sharp at any scale, alpha holds the true distance used for outlines, glows and
// soft shadows. One atlas serves every text size, and glyphs are ordinary prims, so text batches with shapes.
//
// Typical use:
//   FontAtlas atlas;
//   Font* ui = atlas.AddFontDefault();                       // or AddFontFromFile("MyFont.ttf")
//   atlas.Build();
//   atlas.SetTexture(renderer.CreateTexture(atlas.Width(), atlas.Height(), atlas.Pixels()));
//   atlas.ReleasePixels();
//   ...
//   drawList.AddText(*ui, 18.0f, {20, 20}, colors::White, "Hello");
#pragma once

#include "drizzy/core.h"

#include <memory>
#include <string_view>
#include <vector>

namespace drizzy {

class FontAtlas;

// Inclusive range of Unicode code points.
struct GlyphRange {
    uint32_t first = 0;
    uint32_t last = 0;
};

struct FontConfig {
    // Glyphs to generate. The default covers English and Western European text (Basic Latin + Latin-1 Supplement).
    std::vector<GlyphRange> ranges = {{0x20, 0x7E}, {0xA0, 0xFF}};
    // Em size in atlas pixels. Larger keeps fine detail when text is drawn very large, at the cost of atlas memory.
    float atlasEmSize = 48.0f;
    // Distance field range in atlas pixels. Bounds the maximum outline / glow width; see TextStyle.
    float distanceRange = 8.0f;
};

struct Glyph {
    uint32_t codepoint = 0;
    float advance = 0.0f;                          // em
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;  // quad relative to the pen on the baseline, em, +y down
    float u0 = 0.0f, v0 = 0.0f, u1 = 0.0f, v1 = 0.0f;  // atlas texture coordinates
    uint32_t uvPacked[2] = {};                     // (u0, v0) and (u1, v1) as unorm16 pairs, as GpuPrim stores them
    bool visible = false;                          // false for whitespace and control characters
};

class Font {
public:
    ~Font();
    Font(const Font&) = delete;
    Font& operator=(const Font&) = delete;

    // Vertical metrics in em; multiply by the size in pixels.
    float Ascent() const { return m_ascent; }          // above the baseline
    float Descent() const { return m_descent; }        // below the baseline, positive
    float LineHeight() const { return m_lineHeight; }  // baseline to baseline

    // The glyph for `codepoint`, or the fallback glyph ('?') when the font or the configured ranges lack it.
    const Glyph& GetGlyph(uint32_t codepoint) const {
        return codepoint < 128 ? m_glyphs[m_ascii[codepoint]] : GetGlyphSlow(codepoint);
    }
    bool HasGlyph(uint32_t codepoint) const;
    // Kerning adjustment between two code points, in em.
    float Kerning(uint32_t left, uint32_t right) const {
        if ((left | right) < 128) return AsciiKerning(left, right);
        return m_hasKerning ? KerningSlow(left, right) : 0.0f;
    }
    // Pen advance from `prev` (0 at the start of a line) to and across `c`, in em: kerning plus c's advance.
    float Advance(uint32_t prev, uint32_t c) const {
        if ((prev | c) < 128) return m_asciiAdvance[c] + AsciiKerning(prev, c);
        return (prev ? Kerning(prev, c) : 0.0f) + GetGlyph(c).advance;
    }

    // Size of the block DrawList::AddText draws for the same arguments.
    Vec2 MeasureText(std::string_view text, float size, float wrapWidth = 0.0f, float lineSpacing = 1.0f) const;

    const FontConfig& Config() const { return m_config; }
    const FontAtlas& Atlas() const { return *m_atlas; }
    TextureId Texture() const;

private:
    friend class FontAtlas;
    struct Source;

    Font(FontAtlas* atlas, std::vector<uint8_t>&& data, const FontConfig& config);
    const Glyph& GetGlyphSlow(uint32_t codepoint) const;
    float KerningSlow(uint32_t left, uint32_t right) const;
    // Pairs of printable ASCII (0x20-0x7F); anything else, including a 0 "no previous character", kerns by nothing.
    float AsciiKerning(uint32_t left, uint32_t right) const {
        const uint32_t l = left - 0x20u, r = right - 0x20u;  // control characters wrap around and fail the test
        return (l < kAsciiKernSize && r < kAsciiKernSize) ? float(m_asciiKerning[l * kAsciiKernSize + r]) * m_unitsToEm
                                                          : 0.0f;
    }
    static constexpr uint32_t kAsciiKernSize = 96;

    FontAtlas* m_atlas = nullptr;
    FontConfig m_config;
    std::vector<uint8_t> m_data;
    std::unique_ptr<Source> m_source;
    float m_ascent = 0.0f, m_descent = 0.0f, m_lineHeight = 0.0f;
    float m_unitsToEm = 0.0f;
    std::vector<Glyph> m_glyphs;  // sorted by code point
    uint32_t m_fallback = 0;      // index into m_glyphs
    uint32_t m_ascii[128] = {};   // code point -> index into m_glyphs
    float m_asciiAdvance[128] = {};  // code point -> GetGlyph(c).advance, small enough to stay in L1 while measuring
    bool m_hasKerning = false;  // any non-ASCII pairs (the ASCII table is simply zero without kerning)
    int16_t m_asciiKerning[kAsciiKernSize * kAsciiKernSize] = {};  // printable ASCII pairs, font units
    std::vector<std::pair<uint64_t, float>> m_kerningPairs;  // (left << 32 | right) -> em, sorted; non-ASCII pairs
};

class FontAtlas {
public:
    FontAtlas();
    ~FontAtlas();
    FontAtlas(const FontAtlas&) = delete;
    FontAtlas& operator=(const FontAtlas&) = delete;

    // Adds a TrueType / OpenType font from memory (the data is copied). Returns null if it cannot be parsed.
    Font* AddFont(const void* data, size_t size, const FontConfig& config = {});
    Font* AddFontFromFile(const char* path, const FontConfig& config = {});
    // Inter Regular (SIL Open Font License), embedded when built with DRIZZY_EMBED_DEFAULT_FONT. Null otherwise.
    Font* AddFontDefault(const FontConfig& config = {});

    // Generates every glyph of every added font (on all CPU cores) and packs them into one image.
    bool Build();

    // RGBA8 image produced by Build(). Upload it, then hand the texture to SetTexture.
    const uint8_t* Pixels() const { return m_pixels.empty() ? nullptr : m_pixels.data(); }
    uint32_t Width() const { return m_width; }
    uint32_t Height() const { return m_height; }
    void ReleasePixels();

    void SetTexture(TextureId texture) { m_texture = texture; }
    TextureId Texture() const { return m_texture; }

    uint32_t FontCount() const { return uint32_t(m_fonts.size()); }
    Font* GetFont(uint32_t index) const { return m_fonts[index].get(); }

private:
    std::vector<std::unique_ptr<Font>> m_fonts;
    std::vector<uint8_t> m_pixels;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    TextureId m_texture = kNoTexture;
};

inline TextureId Font::Texture() const { return m_atlas->Texture(); }

} // namespace drizzy
