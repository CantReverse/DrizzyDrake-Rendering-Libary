// drizzy_renderer - draw lists.
//
// A DrawList records shapes for one frame as an array of 32-byte GpuPrim records plus a short list of DrawCmds (one per
// clip rect / texture change). Backends issue one draw call per DrawCmd. The vertex shader expands every prim into a
// quad and the pixel shader evaluates the shape analytically (signed distance fields), so anti-aliasing, rounded
// corners, borders and gradients cost no extra geometry.
//
// Polylines and curves become one Stroke prim per segment: the vertex shader extrudes it along the miters it shares
// with its neighbours (computed on the GPU), and the pixel shader anti-aliases across it. A filled triangle is one
// Polygon prim; larger polygons are triangulated into Polygon prims plus one per edge for the anti-aliasing band.
// Everything flows through the same buffer and draw calls. Text glyphs are prims too, so text and shapes share draw
// calls.
//
// Prims normally live in the list's own memory and backends copy them to the GPU when rendering. A list given a
// PrimAllocator (SetPrimAllocator; backends implement it) instead writes its prims straight into mapped GPU memory, so
// rendering copies nothing.
#pragma once

#include "drizzy/core.h"

#include <string_view>

namespace drizzy {

class Font;

// ---------------------------------------------------------------------------------------------------------------------
// GPU primitive format. Must match src/shaders/prim.hlsl.
//
// Most shapes fit one 32-byte record. Shapes that need more data (gradients, borders on a filled shape, images,
// tessellated quads, outlined text, cut-out shadows) set kExtended and take a second record right behind the first,
// marked kContinuation; the GPU skips continuation records when it expands prims into quads.
// ---------------------------------------------------------------------------------------------------------------------
enum class PrimType : uint32_t {
    Rect = 0,     // rounded rect: optional outline, border, gradient, texture
    Ellipse = 1,  // ellipse / circle: filled or outline
    Line = 2,     // single segment with butt, round or square caps
    Quad = 3,     // four arbitrary corners with per-vertex colors (tessellated geometry); always extended
    Shadow = 4,   // blurred rounded rect, optionally cut out under the shape that casts it
    Glyph = 5,    // text glyph from a multi-channel signed distance field atlas
    Stroke = 6,   // one segment of a polyline, mitered into its neighbours
    Polygon = 7,  // filled triangle or polygon piece: anti-aliased by edge distances (the last free type value)
};

namespace prim_flags {
constexpr uint32_t kTypeMask = 0x7u;
constexpr uint32_t kTextured = 1u << 3;      // multiply by the draw command's texture
constexpr uint32_t kTexAlpha = 1u << 4;      // texture holds coverage in .r
constexpr uint32_t kGradientH = 1u << 5;     // color -> fill end, left to right
constexpr uint32_t kGradientV = 1u << 6;     // color -> fill end, top to bottom
constexpr uint32_t kCapRound = 1u << 7;
constexpr uint32_t kCapSquare = 1u << 8;
constexpr uint32_t kExtended = 1u << 9;      // a continuation record follows
constexpr uint32_t kContinuation = 1u << 10;  // the second record of an extended prim
constexpr uint32_t kOutline = 1u << 11;      // Rect / Ellipse: only the outline, in `color`
constexpr uint32_t kCutout = 1u << 12;       // Shadow: not drawn under the casting shape (needs kExtended)
constexpr uint32_t kStrokeFadeStart = 1u << 13;  // Stroke: anti-aliased across p0 (an open polyline's first point)
constexpr uint32_t kStrokeFadeEnd = 1u << 14;    // Stroke: anti-aliased across p1 (an open polyline's last point)
constexpr uint32_t kPolyModeMask = 3u << 13;     // Polygon: the record's role, one of:
constexpr uint32_t kPolyTriangle = 0u << 13;     //   a lone triangle, every edge anti-aliased
constexpr uint32_t kPolyEdge = 1u << 13;         //   the anti-aliasing band along one edge of a polygon
constexpr uint32_t kPolyInterior = 2u << 13;     //   two triangles of a polygon's inside, up to where the bands begin
constexpr uint32_t kPolyCcw = 1u << 15;          // Polygon: winds counter-clockwise on screen (negative area)
constexpr uint32_t kParamShift = 16;         // bits 16-31: the prim's scalar parameter as fp16
} // namespace prim_flags

// One 32-byte record. Field use by type (fp16x2 = two halves packed low, high):
//
//   type      a                     color        data[0]            data[1]            param (fp16 in flags)
//   Rect      min.xy, max.xy        fill/outline radii tl, tr      radii br, bl       border / outline width
//   Ellipse   min.xy, max.xy        fill/outline -                  -                  outline width
//   Line      p0.xy, p1.xy          color        -                  -                  thickness
//   Shadow    min.xy, max.xy        color        radii tl, tr      radii br, bl       blur radius
//   Glyph     min.xy, max.xy        fill         uv0 (unorm16x2)    uv1 (unorm16x2)    distance range in pixels
//   Quad      v0.xy, v1.xy          color 0      color 1            color 2            -
//   Stroke    p0.xy, p1.xy          color        link to previous   link to next       thickness
//
// A Stroke's links are signed record offsets (int32) to the records of the segments before and after it in the same
// polyline, 0 for none. The vertex shader reads the neighbour's far point and builds the miter there; both segments at
// a point compute that miter from the same three points, so they share their corners exactly and joins neither
// overlap nor gap. The records of one polyline are always contiguous in one buffer.
//
// Polygon records, by role (kPolyModeMask). A filled polygon of n > 3 points is n edge records (edge k runs from
// point k to point k + 1) followed by its interior records; the shader finds every point's neighbours through the edge
// records and pushes the point along its miter, so bands and interior meet exactly. A lone triangle is one record.
//
//   role      a                          color   data[0]          data[1]
//   triangle  v0.xy, v1.xy               fill    v2.x (bits)      v2.y (bits)
//   edge      point k, point k + 1       fill    k                n
//   interior  4 point indices (floats)   fill    link to edge 0   n          (triangles 0 1 2 and 0 2 3)
//
// Continuation record of an extended prim:
//
//   Rect      uv0.xy, uv1.xy        fill end     border color       -
//   Glyph     -                     outline      outline width, softness (fp16x2)
//   Quad      v2.xy, v3.xy          color 3      -                  -
//   Shadow    quad min.xy, max.xy   -            caster offset (fp16x2)        (the quad drawn; kCutout)
struct alignas(16) GpuPrim {
    float a[4];
    uint32_t color;
    uint32_t data[2];
    uint32_t flags;  // PrimType | prim_flags | fp16 param << kParamShift
};
static_assert(sizeof(GpuPrim) == 32, "GpuPrim must match the shader's 32-byte layout");

// A run of consecutive prims sharing a clip rect and texture. Backends issue one draw call per DrawCmd.
struct DrawCmd {
    Rect clipRect;
    TextureId texture = kNoTexture;  // kNoTexture: the run has no textured prims, so any binding works
    uint32_t primOffset = 0;         // index of the first prim in its memory (see `block`)
    uint32_t primCount = 0;          // GpuPrim records, continuation records included
    uint32_t block = 0;              // 0: the list's own memory, DrawList::Prims(); else a PrimAllocator block
};

// GPU-visible memory a PrimAllocator hands to a DrawList.
struct PrimChunk {
    GpuPrim* data = nullptr;  // write-only: often write-combined memory, where reads are extremely slow
    uint32_t count = 0;       // records available
    uint32_t block = 0;       // allocator-defined id of the buffer the chunk lives in; never 0 or 0xFFFFFFFF
    uint32_t offset = 0;      // index of data[0] within that buffer
};

// Hands out memory for DrawLists to record into. Backends implement it over mapped GPU buffers, so a frame's prims
// are written once, directly where the GPU reads them. Memory handed out is valid until the backend's next Render:
// lists using an allocator must be rebuilt every frame (as the UI's lists are).
class PrimAllocator {
public:
    // Memory for at least `minCount` records, ideally `desiredCount`. Returning false makes the list fall back to its
    // own memory for the rest of the frame.
    virtual bool AllocatePrims(uint32_t minCount, uint32_t desiredCount, PrimChunk& chunk) = 0;

protected:
    ~PrimAllocator() = default;
};

// ---------------------------------------------------------------------------------------------------------------------
// Shape parameters
// ---------------------------------------------------------------------------------------------------------------------
enum class LineCap : uint8_t { Butt, Round, Square };
enum class Gradient : uint8_t { None, Horizontal, Vertical };

struct CornerRadii {
    float tl = 0.0f, tr = 0.0f, br = 0.0f, bl = 0.0f;
    constexpr CornerRadii() = default;
    constexpr CornerRadii(float all) : tl(all), tr(all), br(all), bl(all) {}
    constexpr CornerRadii(float tl_, float tr_, float br_, float bl_) : tl(tl_), tr(tr_), br(br_), bl(bl_) {}
};

// Everything a rectangle can do in one prim. The convenience functions on DrawList cover the common cases.
struct RectStyle {
    Color fill = colors::White;
    Color fillEnd = colors::White;  // gradient end color, used when gradient != None
    Gradient gradient = Gradient::None;
    CornerRadii radii;
    float borderWidth = 0.0f;  // drawn inside the rect
    Color borderColor = colors::Transparent;
};

// Text appearance. Effects are evaluated from the font's distance field, so they cost nothing extra per pixel beyond
// one texture sample. Outline width and softness are limited to half the font's distance range at the drawn size
// (FontConfig::distanceRange * size / FontConfig::atlasEmSize / 2): about 2.7px for 32px text with the defaults.
struct TextStyle {
    Color color = colors::White;
    float outlineWidth = 0.0f;  // pixels, grows outward from the glyph edge
    Color outlineColor = colors::Black;
    float softness = 0.0f;      // extra edge blur in pixels (glows)
    Color shadowColor = colors::Transparent;  // drop shadow, drawn under the text when its alpha is non-zero
    Vec2 shadowOffset = {1.0f, 1.0f};
    float shadowSoftness = 1.0f;
    float shadowSpread = 0.0f;  // grows the shadow beyond the glyph (plus outline); zero offset + spread = glow
    float wrapWidth = 0.0f;     // wrap at word boundaries past this width in pixels; 0 disables wrapping
    float lineSpacing = 1.0f;   // multiplier on the font's line height
};

// Per-frame counters reported by backends.
struct RenderStats {
    uint32_t prims = 0;  // GpuPrim records drawn
    uint32_t drawCalls = 0;
};

// How a render target expects its colors. drizzy colors (and textures) are authored in sRGB; on HDR targets they are
// shown at RenderParams::paperWhiteNits, the brightness of SDR white.
enum class ColorEncoding : uint8_t {
    Srgb,        // UNORM target holding sRGB values (SDR back buffers, 8 or 10 bit): written as authored
    SrgbLinear,  // *_SRGB view: written linear, the GPU encodes on write
    ScRgb,       // FP16 scRGB (Windows HDR / wide gamut): linear Rec.709, 1.0 = 80 nits
    Hdr10,       // HDR10: SMPTE ST 2084 (PQ) encoded Rec.2020, typically R10G10B10A2
};

// ---------------------------------------------------------------------------------------------------------------------
// DrawList
//
// Conventions:
//  - Coordinates are pixels, origin top-left, +y down. A 1px line centered on y = 10.5 covers exactly one pixel row.
//  - Outlines of rects, circles and ellipses are drawn inside the shape (like CSS borders); polyline and polygon
//    strokes are centered on the path.
//  - Angles are radians, clockwise on screen, 0 = +x.
//  - Functions that take segment counts use 0 for "automatic", driven by curveTolerance.
//  - Functions that build on the path API (curves, arcs, polygon outlines) clear the current path.
// ---------------------------------------------------------------------------------------------------------------------
class DrawList {
public:
    DrawList();
    DrawList(const DrawList&) = delete;
    DrawList& operator=(const DrawList&) = delete;

    // Starts a new frame. Keeps every allocation, so steady-state frames do not touch the heap.
    void Reset(const Rect& clipRect);

    // Records into memory from `allocator` (null: the list's own memory) from the next Reset on. A list with an
    // allocator must be rendered by the allocator's backend after every Reset, before the backend's next Render.
    void SetPrimAllocator(PrimAllocator* allocator) { m_allocator = allocator; }
    PrimAllocator* GetPrimAllocator() const { return m_allocator; }

    // ---- Clipping ---------------------------------------------------------------------------------------------------
    void PushClipRect(const Rect& rect, bool intersectWithCurrent = true);
    void PopClipRect();
    const Rect& ClipRect() const { return m_clipStack.Back(); }

    // ---- Rectangles -------------------------------------------------------------------------------------------------
    void AddRectFilled(const Rect& r, Color color, float rounding = 0.0f);
    void AddRectFilled(const Rect& r, Color color, const CornerRadii& radii);
    void AddRect(const Rect& r, Color color, float rounding = 0.0f, float thickness = 1.0f);
    void AddRectGradient(const Rect& r, Color from, Color to, Gradient direction, float rounding = 0.0f);
    void AddRectFilledMultiColor(const Rect& r, Color tl, Color tr, Color br, Color bl);
    void AddRectEx(const Rect& r, const RectStyle& style);
    // Soft drop shadow for a rounded rect. Draw it before the shape that casts it. With `cutout`, the shadow is not
    // drawn under the casting rect itself (like CSS box-shadow): what shows through a translucent shape is then what
    // is behind it, and large shadows cost far less GPU time, since only the visible rim is shaded.
    void AddShadow(const Rect& r, Color color, float blur, float rounding = 0.0f, Vec2 offset = {},
                   bool cutout = false);

    // ---- Circles and ellipses ---------------------------------------------------------------------------------------
    void AddCircleFilled(Vec2 center, float radius, Color color);
    void AddCircle(Vec2 center, float radius, Color color, float thickness = 1.0f);
    void AddEllipseFilled(Vec2 center, Vec2 radii, Color color);
    void AddEllipse(Vec2 center, Vec2 radii, Color color, float thickness = 1.0f);

    // ---- Lines and curves -------------------------------------------------------------------------------------------
    void AddLine(Vec2 a, Vec2 b, Color color, float thickness = 1.0f, LineCap cap = LineCap::Butt);
    void AddPolyline(const Vec2* points, uint32_t count, Color color, float thickness = 1.0f, bool closed = false);
    void AddBezierCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, Color color, float thickness = 1.0f,
                        uint32_t segments = 0);
    void AddBezierQuadratic(Vec2 p0, Vec2 p1, Vec2 p2, Color color, float thickness = 1.0f, uint32_t segments = 0);
    void AddArc(Vec2 center, float radius, float angleMin, float angleMax, Color color, float thickness = 1.0f,
                uint32_t segments = 0);

    // ---- Polygons ---------------------------------------------------------------------------------------------------
    void AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Color color);
    void AddTriangle(Vec2 a, Vec2 b, Vec2 c, Color color, float thickness = 1.0f);
    void AddQuadFilled(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color color);  // convex
    void AddQuad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color color, float thickness = 1.0f);
    void AddConvexPolyFilled(const Vec2* points, uint32_t count, Color color);
    void AddConcavePolyFilled(const Vec2* points, uint32_t count, Color color);  // simple (non-self-intersecting)

    // ---- Images -----------------------------------------------------------------------------------------------------
    // Draws a texture (straight alpha) multiplied by `tint`. Rounded corners mask the image.
    void AddImage(TextureId texture, const Rect& r, Vec2 uv0 = {0.0f, 0.0f}, Vec2 uv1 = {1.0f, 1.0f},
                  Color tint = colors::White, float rounding = 0.0f);

    // ---- Text -------------------------------------------------------------------------------------------------------
    // Draws UTF-8 text at `size` pixels per em. `pos` is the top-left of the first line; the baseline is snapped to
    // whole pixels. '\n' starts a new line. Returns the size of the text block (same as Font::MeasureText).
    Vec2 AddText(const Font& font, float size, Vec2 pos, Color color, std::string_view text);
    Vec2 AddText(const Font& font, float size, Vec2 pos, std::string_view text, const TextStyle& style);
    // Draws text clipped to `clip` (within the current clip rect) by trimming its glyphs on the CPU, so a clipped label
    // needs no clip rect of its own, and no extra draw command. Returns nothing: it stops reading a line once the line
    // has left the clip rect (Font::MeasureText gives the size).
    void AddTextClipped(const Font& font, float size, Vec2 pos, Color color, std::string_view text, const Rect& clip);

    // ---- Path API ---------------------------------------------------------------------------------------------------
    void PathClear() { m_path.Clear(); }
    void PathLineTo(Vec2 p) { m_path.PushBack(p); }
    void PathArcTo(Vec2 center, float radius, float angleMin, float angleMax, uint32_t segments = 0);
    void PathBezierCubicTo(Vec2 p1, Vec2 p2, Vec2 p3, uint32_t segments = 0);
    void PathBezierQuadraticTo(Vec2 p1, Vec2 p2, uint32_t segments = 0);
    void PathStroke(Color color, float thickness = 1.0f, bool closed = false);
    void PathFillConvex(Color color);
    void PathFillConcave(Color color);

    // ---- Combining lists --------------------------------------------------------------------------------------------
    // Appends `other`'s prims (it must use its own memory, no allocator) and its commands, as if they had been drawn
    // here; drawing then continues under this list's current clip rect. The UI draws each table column into its own
    // list and appends them, so a table costs one draw call per column instead of one per cell.
    void AppendList(const DrawList& other);

    // ---- Output, consumed by backends -------------------------------------------------------------------------------
    // Records in the list's own memory (what DrawCmds with block 0 index into). Without an allocator that is every
    // record of the frame.
    const GpuPrim* Prims() const { return m_prims.Data(); }
    uint32_t OwnPrimCount() const { return m_writeBlock == 0 ? uint32_t(m_write - m_prims.Data()) : 0u; }
    // Records of the frame wherever they live, continuation records included.
    uint32_t PrimCount() const { return m_primCount; }
    const DrawCmd* Cmds() const { return m_cmds.Data(); }
    uint32_t CmdCount() const { return m_cmds.Size(); }

    // Maximum distance in pixels between a curve and its tessellation.
    float curveTolerance = 0.25f;
    // Target pixels per list unit, when the list is drawn scaled (DrawData::scale; the UI's lists at a UI scale).
    // Tessellated geometry uses it to keep its anti-aliasing fringe one target pixel wide.
    float pixelScale = 1.0f;

private:
    // Room for `count` records under the current command, adopting or switching to `texture` when it is set.
    DZ_FORCEINLINE GpuPrim* AllocPrims(TextureId texture, uint32_t count) {
        if (texture != kNoTexture && m_cmds.Back().texture != texture) UseTexture(texture);
        if (size_t(m_writeEnd - m_write) < count) NextChunk(count);
        GpuPrim* p = m_write;
        m_write += count;
        m_cmds.Back().primCount += count;
        m_primCount += count;
        return p;
    }
    // Gives back the last `count` records of the latest AllocPrims: reserved for the worst case, left unwritten.
    DZ_FORCEINLINE void ReturnPrims(uint32_t count) {
        m_write -= count;
        m_cmds.Back().primCount -= count;
        m_primCount -= count;
    }
    uint32_t WriteOffset() const { return m_chunkOffset + uint32_t(m_write - m_chunkBase); }
    // One target pixel, and the curve tolerance, in list units.
    float FringeWidth() const { return pixelScale > 0.0f ? 1.0f / pixelScale : 1.0f; }
    float CurveTolerance() const {
        const float t = curveTolerance * FringeWidth();
        return t > 0.01f ? t : 0.01f;
    }
    void UseTexture(TextureId texture);
    void NextChunk(uint32_t count);
    void StartCmd(const Rect& clip, TextureId texture);
    bool Culled(const Rect& bounds) const;
    void SetClipRect(const Rect& rect);
    void PushQuad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color ca, Color cb, Color cc, Color cd);
    uint32_t ArcSegments(float radius, float sweep) const;
    uint32_t PreparePolygon(const Vec2* points, uint32_t count, bool closed, uint32_t extraArrays);
    void StrokePolyline(const Vec2* points, uint32_t count, Color color, float thickness, bool closed);
    void FillPolygon(const Vec2* points, uint32_t count, Color color, bool convex);
    Vec2 EmitText(const Font& font, float size, Vec2 pos, std::string_view text, float wrapWidth, float lineSpacing,
                  Color fill, Color outline, float outlineWidth, float softness, const Rect* fineClip);

    // Where records go: [m_write, m_writeEnd) in a chunk that starts at m_chunkBase, record m_chunkOffset of block
    // m_writeBlock (0 = m_prims).
    GpuPrim* m_write = nullptr;
    GpuPrim* m_writeEnd = nullptr;
    GpuPrim* m_chunkBase = nullptr;
    uint32_t m_chunkOffset = 0;
    uint32_t m_writeBlock = 0;
    uint32_t m_primCount = 0;      // records this frame
    uint32_t m_lastPrimCount = 0;  // records last frame: sizes the first chunk
    bool m_allocatorFailed = false;
    PrimAllocator* m_allocator = nullptr;

    PodArray<GpuPrim> m_prims;  // own memory; only its capacity is used, records are tracked by m_write
    PodArray<DrawCmd> m_cmds;
    PodArray<Rect> m_clipStack;
    PodArray<Vec2> m_path;
    PodArray<Vec2> m_scratch;  // tessellation temporaries
    PodArray<uint32_t> m_scratchIndices;
    PodArray<uint32_t> m_triangles;
};

// Everything a backend needs to render one frame. Lists are drawn in order, each on top of the previous one.
struct DrawData {
    const DrawList* const* lists = nullptr;
    uint32_t listCount = 0;
    Vec2 displaySize;    // render target size in pixels
    float scale = 1.0f;  // target pixels per list unit: list coordinates are multiplied by it (the UI scale)

    uint32_t TotalPrimCount() const {
        uint32_t total = 0;
        for (uint32_t i = 0; i < listCount; ++i) total += lists[i]->PrimCount();
        return total;
    }
};

} // namespace drizzy
