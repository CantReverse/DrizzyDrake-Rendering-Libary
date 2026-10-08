// Font loading and MTSDF atlas generation. Outlines are read with stb_truetype and turned into distance fields with
// msdfgen; both are compiled into drizzy_core, so there is nothing to ship at runtime.
//
// stb_truetype does not validate fonts defensively: only load font files you ship, not ones supplied by players.
#include "drizzy/font.h"

#include "text_layout.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <thread>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#if defined(_MSC_VER)
#pragma warning(disable : 4505)  // unused stb_truetype functions with internal linkage
#endif
#include "stb_truetype.h"

#include "msdfgen.h"
#include "core/ShapeDistanceFinder.h"

#if defined(DRIZZY_HAS_DEFAULT_FONT)
extern const unsigned char kDrizzyDefaultFont[];
extern const size_t kDrizzyDefaultFontSize;
#endif

namespace drizzy {

struct Font::Source {
    stbtt_fontinfo info = {};
};

namespace {

constexpr float kTabWidthInSpaces = 4.0f;
constexpr uint32_t kMaxAtlasSize = 8192;
constexpr uint32_t kMaxGlyphsPerFont = 65536;

template <typename Fn>
void ParallelFor(size_t count, Fn&& fn) {
    const unsigned hw = std::thread::hardware_concurrency();
    const size_t threads = std::min<size_t>(count, hw ? hw : 4);
    if (threads <= 1) {
        for (size_t i = 0; i < count; ++i) fn(i);
        return;
    }
    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count;) fn(i);
    };
    std::vector<std::thread> pool;
    pool.reserve(threads - 1);
    for (size_t t = 1; t < threads; ++t) pool.emplace_back(worker);
    worker();
    for (std::thread& t : pool) t.join();
}

// One glyph to turn into a distance field.
struct GlyphJob {
    const stbtt_fontinfo* info = nullptr;
    int glyphIndex = 0;
    double unitsToPx = 0.0;  // font units -> atlas pixels
    float distanceRange = 0.0f;
    float atlasEmSize = 0.0f;
    Glyph* glyph = nullptr;
    // Results. The bitmap's bottom-left corner sits at (originX, originY) atlas pixels from the glyph origin, +y up.
    int width = 0, height = 0;
    double originX = 0.0, originY = 0.0;
    std::vector<uint8_t> pixels;  // RGBA8, top row first
    uint32_t atlasX = 0, atlasY = 0;
};

bool BuildShape(const stbtt_fontinfo& info, int glyphIndex, msdfgen::Shape& shape) {
    stbtt_vertex* verts = nullptr;
    const int count = stbtt_GetGlyphShape(&info, glyphIndex, &verts);
    msdfgen::Contour* contour = nullptr;
    msdfgen::Point2 cursor;
    for (int i = 0; i < count; ++i) {
        const stbtt_vertex& v = verts[i];
        const msdfgen::Point2 p(v.x, v.y);
        if (v.type == STBTT_vmove) {
            contour = &shape.addContour();
        } else if (contour && p != cursor) {
            if (v.type == STBTT_vline) {
                contour->addEdge(msdfgen::EdgeHolder(cursor, p));
            } else if (v.type == STBTT_vcurve) {
                contour->addEdge(msdfgen::EdgeHolder(cursor, msdfgen::Point2(v.cx, v.cy), p));
            } else if (v.type == STBTT_vcubic) {
                contour->addEdge(
                    msdfgen::EdgeHolder(cursor, msdfgen::Point2(v.cx, v.cy), msdfgen::Point2(v.cx1, v.cy1), p));
            }
        }
        cursor = p;
    }
    stbtt_FreeShape(&info, verts);
    // Drop contours that ended up without edges (e.g. a lone move-to).
    shape.contours.erase(std::remove_if(shape.contours.begin(), shape.contours.end(),
                                        [](const msdfgen::Contour& c) { return c.edges.empty(); }),
                         shape.contours.end());
    return !shape.contours.empty();
}

void RasterizeGlyph(GlyphJob& job) {
    msdfgen::Shape shape;
    if (!BuildShape(*job.info, job.glyphIndex, shape)) return;
    shape.normalize();
    const msdfgen::Shape::Bounds b = shape.getBounds();
    if (!(b.l < b.r && b.b < b.t)) return;

    // TrueType and CFF outlines wind in opposite directions. msdfgen needs "outside" to be negative, so flip the
    // contours if a point well outside the glyph reads as inside.
    const msdfgen::Point2 outside(b.l - (b.r - b.l) - 1.0, b.b - (b.t - b.b) - 1.0);
    if (msdfgen::SimpleTrueShapeDistanceFinder::oneShotDistance(shape, outside) > 0.0) {
        for (msdfgen::Contour& c : shape.contours) c.reverse();
    }
    msdfgen::edgeColoringInkTrap(shape, 3.0);

    // Pixel-aligned bitmap covering the outline plus half the distance range on every side.
    const double scale = job.unitsToPx;
    const double pad = job.distanceRange * 0.5;
    const double x0 = std::floor(b.l * scale - pad), y0 = std::floor(b.b * scale - pad);
    const double x1 = std::ceil(b.r * scale + pad), y1 = std::ceil(b.t * scale + pad);
    const int width = int(x1 - x0), height = int(y1 - y0);
    if (width <= 0 || height <= 0 || width > int(kMaxAtlasSize) || height > int(kMaxAtlasSize)) return;

    msdfgen::Bitmap<float, 4> bitmap(width, height);
    const msdfgen::SDFTransformation transform(
        msdfgen::Projection(msdfgen::Vector2(scale), msdfgen::Vector2(-x0 / scale, -y0 / scale)),
        msdfgen::DistanceMapping(msdfgen::Range(job.distanceRange / scale)));
    msdfgen::generateMTSDF(bitmap, shape, transform);

    job.width = width;
    job.height = height;
    job.originX = x0;
    job.originY = y0;
    job.pixels.resize(size_t(width) * size_t(height) * 4);
    for (int y = 0; y < height; ++y) {
        uint8_t* dst = &job.pixels[size_t(y) * size_t(width) * 4];
        for (int x = 0; x < width; ++x) {
            const float* src = bitmap(x, height - 1 - y);  // msdfgen rows run bottom-up
            for (int ch = 0; ch < 4; ++ch) dst[x * 4 + ch] = uint8_t(Clamp(src[ch], 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    }
}

// Fills in the font's metrics, glyph table and kerning, and returns the stb_truetype glyph index of each glyph.
std::vector<int> PrepareGlyphs(const stbtt_fontinfo& info, const FontConfig& config, float& unitsToEm, float& ascent,
                               float& descent, float& lineHeight, std::vector<Glyph>& glyphs) {
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    unitsToEm = stbtt_ScaleForMappingEmToPixels(&info, 1.0f);
    ascent = float(asc) * unitsToEm;
    descent = float(-desc) * unitsToEm;
    lineHeight = float(asc - desc + gap) * unitsToEm;

    // Control characters get synthetic invisible glyphs (tab = 4 spaces) so they never show the fallback.
    std::vector<uint32_t> codepoints;
    for (uint32_t c = 0; c < 0x20; ++c) codepoints.push_back(c);
    codepoints.push_back(' ');
    codepoints.push_back('?');
    for (const GlyphRange& r : config.ranges) {
        for (uint32_t c = std::max(r.first, 0x20u); c <= std::min(r.last, 0x10FFFFu); ++c) {
            if (codepoints.size() >= kMaxGlyphsPerFont) break;
            codepoints.push_back(c);
        }
    }
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());

    int spaceAdvance = 0;
    stbtt_GetGlyphHMetrics(&info, stbtt_FindGlyphIndex(&info, ' '), &spaceAdvance, nullptr);

    glyphs.clear();
    std::vector<int> indices;
    for (uint32_t c : codepoints) {
        Glyph g;
        g.codepoint = c;
        int index = 0;
        if (c < 0x20) {
            g.advance = c == '\t' ? float(spaceAdvance) * unitsToEm * kTabWidthInSpaces : 0.0f;
        } else {
            index = stbtt_FindGlyphIndex(&info, int(c));
            if (index == 0) continue;  // not in the font: lookups fall back
            int advance = 0;
            stbtt_GetGlyphHMetrics(&info, index, &advance, nullptr);
            g.advance = float(advance) * unitsToEm;
            g.visible = !stbtt_IsGlyphEmpty(&info, index);
        }
        glyphs.push_back(g);
        indices.push_back(index);
    }
    return indices;
}

// `ascii` is a table of `asciiSize` x `asciiSize` pairs of code points 0x20 and up; other pairs go to `pairs`.
void PrepareKerning(const stbtt_fontinfo& info, const std::vector<Glyph>& glyphs, const std::vector<int>& indices,
                    float unitsToEm, int16_t* ascii, uint32_t asciiSize,
                    std::vector<std::pair<uint64_t, float>>& pairs, bool& hasKerning) {
    std::fill(ascii, ascii + size_t(asciiSize) * asciiSize, int16_t(0));
    pairs.clear();
    hasKerning = false;
    if (!info.kern && !info.gpos) return;

    // Only printable glyphs take part in kerning.
    std::vector<size_t> printable;
    for (size_t i = 0; i < glyphs.size(); ++i) {
        if (indices[i] != 0) printable.push_back(i);
    }
    std::vector<std::vector<std::pair<uint64_t, float>>> rows(printable.size());
    ParallelFor(printable.size(), [&](size_t li) {
        const size_t l = printable[li];
        for (size_t r : printable) {
            const int k = stbtt_GetGlyphKernAdvance(&info, indices[l], indices[r]);
            if (k == 0) continue;
            const uint32_t a = glyphs[l].codepoint, b = glyphs[r].codepoint;
            const uint32_t ia = a - 0x20u, ib = b - 0x20u;
            if (ia < asciiSize && ib < asciiSize) {
                ascii[ia * asciiSize + ib] = int16_t(k);  // each (a, b) cell is written by exactly one worker
            } else if ((a | b) >= 128) {
                rows[li].push_back({(uint64_t(a) << 32) | b, float(k) * unitsToEm});
            }
        }
    });
    for (auto& row : rows) pairs.insert(pairs.end(), row.begin(), row.end());
    std::sort(pairs.begin(), pairs.end());
    hasKerning = !pairs.empty();
}

} // namespace

// =====================================================================================================================
// Font
// =====================================================================================================================
Font::Font(FontAtlas* atlas, std::vector<uint8_t>&& data, const FontConfig& config)
    : m_atlas(atlas), m_config(config), m_data(std::move(data)), m_source(std::make_unique<Source>()) {
    m_config.atlasEmSize = Clamp(m_config.atlasEmSize, 8.0f, 512.0f);
    m_config.distanceRange = Clamp(m_config.distanceRange, 1.0f, m_config.atlasEmSize);
    m_glyphs.resize(1);  // lookups stay valid (and draw nothing) until the atlas is built
}

Font::~Font() = default;

// =====================================================================================================================
// FontAtlas
// =====================================================================================================================
FontAtlas::FontAtlas() = default;
FontAtlas::~FontAtlas() = default;

Font* FontAtlas::AddFont(const void* data, size_t size, const FontConfig& config) {
    if (!data || size == 0) return nullptr;
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::unique_ptr<Font> font(new Font(this, std::vector<uint8_t>(bytes, bytes + size), config));
    const int offset = stbtt_GetFontOffsetForIndex(font->m_data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(&font->m_source->info, font->m_data.data(), offset)) return nullptr;
    m_fonts.push_back(std::move(font));
    return m_fonts.back().get();
}

Font* FontAtlas::AddFontFromFile(const char* path, const FontConfig& config) {
    FILE* f = nullptr;
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0) f = nullptr;
#else
    f = std::fopen(path, "rb");
#endif
    if (!f) return nullptr;
    std::vector<uint8_t> data;
    if (std::fseek(f, 0, SEEK_END) == 0) {
        const long size = std::ftell(f);
        if (size > 0 && std::fseek(f, 0, SEEK_SET) == 0) {
            data.resize(size_t(size));
            if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
    }
    std::fclose(f);
    return data.empty() ? nullptr : AddFont(data.data(), data.size(), config);
}

Font* FontAtlas::AddFontDefault(const FontConfig& config) {
#if defined(DRIZZY_HAS_DEFAULT_FONT)
    return AddFont(kDrizzyDefaultFont, kDrizzyDefaultFontSize, config);
#else
    (void)config;
    return nullptr;
#endif
}

void FontAtlas::ReleasePixels() {
    m_pixels.clear();
    m_pixels.shrink_to_fit();
}

bool FontAtlas::Build() {
    m_pixels.clear();
    m_width = m_height = 0;
    if (m_fonts.empty()) return false;

    // 1. Glyph tables, metrics and kerning for every font.
    std::vector<GlyphJob> jobs;
    for (const std::unique_ptr<Font>& fontPtr : m_fonts) {
        Font& font = *fontPtr;
        const stbtt_fontinfo& info = font.m_source->info;
        const std::vector<int> indices = PrepareGlyphs(info, font.m_config, font.m_unitsToEm, font.m_ascent,
                                                       font.m_descent, font.m_lineHeight, font.m_glyphs);
        PrepareKerning(info, font.m_glyphs, indices, font.m_unitsToEm, font.m_asciiKerning, Font::kAsciiKernSize,
                       font.m_kerningPairs, font.m_hasKerning);

        const auto find = [&](uint32_t cp) -> int64_t {
            const auto it = std::lower_bound(font.m_glyphs.begin(), font.m_glyphs.end(), cp,
                                             [](const Glyph& g, uint32_t c) { return g.codepoint < c; });
            return (it != font.m_glyphs.end() && it->codepoint == cp) ? it - font.m_glyphs.begin() : -1;
        };
        const int64_t question = find('?');
        font.m_fallback = uint32_t(question >= 0 ? question : 0);
        for (uint32_t c = 0; c < 128; ++c) {
            const int64_t i = find(c);
            font.m_ascii[c] = uint32_t(i >= 0 ? i : font.m_fallback);
            font.m_asciiAdvance[c] = font.m_glyphs[font.m_ascii[c]].advance;
        }

        const double unitsToPx = double(font.m_unitsToEm) * font.m_config.atlasEmSize;
        for (size_t i = 0; i < font.m_glyphs.size(); ++i) {
            if (!font.m_glyphs[i].visible) continue;
            GlyphJob job;
            job.info = &info;
            job.glyphIndex = indices[i];
            job.unitsToPx = unitsToPx;
            job.distanceRange = font.m_config.distanceRange;
            job.atlasEmSize = font.m_config.atlasEmSize;
            job.glyph = &font.m_glyphs[i];
            jobs.push_back(std::move(job));
        }
    }

    // 2. Distance fields, one glyph per task across all cores.
    ParallelFor(jobs.size(), [&](size_t i) { RasterizeGlyph(jobs[i]); });

    // 3. Shelf-pack tallest first, with a 1px gap so bilinear filtering never reads a neighbour.
    std::vector<GlyphJob*> order;
    double area = 0.0;
    for (GlyphJob& job : jobs) {
        if (job.width == 0) {
            job.glyph->visible = false;
            continue;
        }
        order.push_back(&job);
        area += double(job.width + 1) * double(job.height + 1);
    }
    std::sort(order.begin(), order.end(), [](const GlyphJob* a, const GlyphJob* b) {
        return a->height != b->height ? a->height > b->height : a->width > b->width;
    });
    uint32_t width = 256;
    while (double(width) * double(width) < area * 1.15 && width < kMaxAtlasSize) width *= 2;
    uint32_t x = 0, y = 0, shelfHeight = 0;
    for (GlyphJob* job : order) {
        if (x + uint32_t(job->width) > width) {
            y += shelfHeight + 1;
            x = 0;
            shelfHeight = 0;
        }
        job->atlasX = x;
        job->atlasY = y;
        x += uint32_t(job->width) + 1;
        shelfHeight = std::max(shelfHeight, uint32_t(job->height));
    }
    const uint32_t height = std::max(4u, (y + shelfHeight + 3) & ~3u);
    if (height > kMaxAtlasSize) return false;

    // 4. Copy into the atlas and resolve each glyph's quad and texture coordinates.
    m_width = width;
    m_height = height;
    m_pixels.assign(size_t(width) * height * 4, 0);
    for (GlyphJob* job : order) {
        for (int row = 0; row < job->height; ++row) {
            std::memcpy(&m_pixels[(size_t(job->atlasY + row) * width + job->atlasX) * 4],
                        &job->pixels[size_t(row) * job->width * 4], size_t(job->width) * 4);
        }
        Glyph& g = *job->glyph;
        const double em = job->atlasEmSize;
        g.x0 = float(job->originX / em);
        g.x1 = float((job->originX + job->width) / em);
        g.y0 = float(-(job->originY + job->height) / em);
        g.y1 = float(-job->originY / em);
        g.u0 = float(job->atlasX) / float(width);
        g.v0 = float(job->atlasY) / float(height);
        g.u1 = float(job->atlasX + job->width) / float(width);
        g.v1 = float(job->atlasY + job->height) / float(height);
        g.uvPacked[0] = detail::PackUnorm16x2(g.u0, g.v0);
        g.uvPacked[1] = detail::PackUnorm16x2(g.u1, g.v1);
    }
    return true;
}

} // namespace drizzy
