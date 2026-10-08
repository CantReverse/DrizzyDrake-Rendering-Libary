#include "drizzy/draw_list.h"

#include "drizzy/font.h"
#include "text_layout.h"

namespace drizzy {
namespace {

using namespace prim_flags;

constexpr float kAA = 1.0f;        // anti-aliasing margin of SDF prims (the shader's AA_MARGIN), in list units
constexpr float kMaxMiter = 4.0f;  // miter length limit for joins and corners, in half widths (prim.hlsl MAX_MITER)
constexpr uint32_t kMaxCurveSegments = 512;
constexpr float kDuplicateDistSq = 1e-8f;
constexpr Rect kUnclipped(-1e7f, -1e7f, 1e7f, 1e7f);
constexpr uint32_t kNoBlock = 0xFFFFFFFFu;    // allocator mode before the first chunk of a frame
constexpr uint32_t kMinChunk = 256;           // records
constexpr float kMinCutoutInterior = 64.0f;  // cut-out shadows split into rim strips when the cut is this big (px)

uint16_t FloatToHalf(float f) {
    uint32_t x;
    std::memcpy(&x, &f, sizeof(x));
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int32_t exponent = int32_t((x >> 23) & 0xFFu) - 127 + 15;
    if (exponent <= 0) return uint16_t(sign);             // values below fp16's normal range are irrelevant here
    if (exponent >= 31) return uint16_t(sign | 0x7BFFu);  // clamp to the largest finite half
    const uint32_t mantissa = x & 0x7FFFFFu;
    return uint16_t(sign | ((uint32_t(exponent) << 10) + ((mantissa + 0x1000u) >> 13)));
}

DZ_FORCEINLINE uint32_t PackHalf2(float lo, float hi) {
    return uint32_t(FloatToHalf(lo)) | (uint32_t(FloatToHalf(hi)) << 16);
}

// PrimType, flags and the fp16 parameter in one flags word.
DZ_FORCEINLINE uint32_t PrimFlags(PrimType type, uint32_t flags, float param = 0.0f) {
    return uint32_t(type) | flags | (uint32_t(FloatToHalf(param)) << kParamShift);
}

DZ_FORCEINLINE float MaxRadius(const Rect& r) {
    const float m = 0.5f * (r.Width() < r.Height() ? r.Width() : r.Height());
    return m > 0.0f ? m : 0.0f;
}

DZ_FORCEINLINE void SetRadii(GpuPrim* p, float radius, const Rect& r) {
    if (radius <= 0.0f) {
        p->data[0] = 0;
        p->data[1] = 0;
        return;
    }
    const float maxR = MaxRadius(r);
    const uint32_t h = FloatToHalf(radius < maxR ? radius : maxR);
    p->data[0] = h | (h << 16);
    p->data[1] = h | (h << 16);
}

DZ_FORCEINLINE void SetRadii(GpuPrim* p, const CornerRadii& radii, const Rect& r) {
    const float maxR = MaxRadius(r);
    auto c = [maxR](float v) { return Clamp(v, 0.0f, maxR); };
    p->data[0] = PackHalf2(c(radii.tl), c(radii.tr));
    p->data[1] = PackHalf2(c(radii.br), c(radii.bl));
}

DZ_FORCEINLINE void SetBox(GpuPrim* p, const Rect& r) {
    p->a[0] = r.min.x;
    p->a[1] = r.min.y;
    p->a[2] = r.max.x;
    p->a[3] = r.max.y;
}

// Fills the continuation record of an extended prim. Every field is written: the record may be write-combined memory.
DZ_FORCEINLINE void SetContinuation(GpuPrim* e, float a0, float a1, float a2, float a3, uint32_t color,
                                    uint32_t data0) {
    e->a[0] = a0;
    e->a[1] = a1;
    e->a[2] = a2;
    e->a[3] = a3;
    e->color = color;
    e->data[0] = data0;
    e->data[1] = 0;
    e->flags = kContinuation;
}

constexpr uint32_t GradientFlags(Gradient g) {
    return g == Gradient::Horizontal ? kGradientH : g == Gradient::Vertical ? kGradientV : 0u;
}

Rect BoundsOf(const Vec2* pts, uint32_t count) {
    Rect r(pts[0], pts[0]);
    for (uint32_t i = 1; i < count; ++i) {
        r.min = MinV(r.min, pts[i]);
        r.max = MaxV(r.max, pts[i]);
    }
    return r;
}

// Twice the signed area. Positive for polygons that wind clockwise on screen (+y down).
float SignedArea2(const Vec2* pts, uint32_t count) {
    float area = 0.0f;
    for (uint32_t i = 0, j = count - 1; i < count; j = i++) area += Cross(pts[j], pts[i]);
    return area;
}

bool PointInTriangle(Vec2 p, Vec2 a, Vec2 b, Vec2 c, float sign) {
    return sign * Cross(b - a, p - a) >= 0.0f && sign * Cross(c - b, p - b) >= 0.0f &&
           sign * Cross(a - c, p - c) >= 0.0f;
}

// Ear-clipping triangulation of a simple polygon. `sign` is the sign of the polygon's signed area. Self-intersecting
// input still terminates: when a full lap finds no ear, the current vertex is clipped anyway.
void Triangulate(const Vec2* pts, uint32_t count, float sign, PodArray<uint32_t>& work, PodArray<uint32_t>& tris) {
    work.Resize(count);
    uint32_t* poly = work.Data();
    for (uint32_t i = 0; i < count; ++i) poly[i] = i;
    tris.Clear();

    uint32_t remaining = count;
    uint32_t cursor = 0;
    uint32_t sinceLastEar = 0;
    while (remaining > 3) {
        const uint32_t ia = poly[cursor == 0 ? remaining - 1 : cursor - 1];
        const uint32_t ib = poly[cursor];
        const uint32_t ic = poly[cursor + 1 == remaining ? 0 : cursor + 1];
        bool ear = sign * Cross(pts[ib] - pts[ia], pts[ic] - pts[ib]) > 0.0f;
        for (uint32_t k = 0; ear && k < remaining; ++k) {
            const uint32_t v = poly[k];
            if (v != ia && v != ib && v != ic && PointInTriangle(pts[v], pts[ia], pts[ib], pts[ic], sign)) ear = false;
        }
        if (ear || sinceLastEar >= remaining) {
            uint32_t* t = tris.Append(3);
            t[0] = ia;
            t[1] = ib;
            t[2] = ic;
            std::memmove(poly + cursor, poly + cursor + 1, (remaining - cursor - 1) * sizeof(uint32_t));
            --remaining;
            if (cursor >= remaining) cursor = 0;
            sinceLastEar = 0;
        } else {
            cursor = cursor + 1 == remaining ? 0 : cursor + 1;
            ++sinceLastEar;
        }
    }
    uint32_t* t = tris.Append(3);
    t[0] = poly[0];
    t[1] = poly[1];
    t[2] = poly[2];
}

// Trims a glyph quad to `clip`, moving its texture coordinates with it (they are affine in the position). False when
// nothing of it is left.
bool TrimGlyph(const Glyph& g, const Rect& clip, float& x0, float& y0, float& x1, float& y1, uint32_t& uv0,
               uint32_t& uv1) {
    if (x0 >= clip.min.x && y0 >= clip.min.y && x1 <= clip.max.x && y1 <= clip.max.y) return true;  // inside
    const float cx0 = std::fmax(x0, clip.min.x), cy0 = std::fmax(y0, clip.min.y);
    const float cx1 = std::fmin(x1, clip.max.x), cy1 = std::fmin(y1, clip.max.y);
    if (!(cx0 < cx1 && cy0 < cy1)) return false;
    const float su = (g.u1 - g.u0) / (x1 - x0), sv = (g.v1 - g.v0) / (y1 - y0);
    uv0 = detail::PackUnorm16x2(g.u0 + (cx0 - x0) * su, g.v0 + (cy0 - y0) * sv);
    uv1 = detail::PackUnorm16x2(g.u1 - (x1 - cx1) * su, g.v1 - (y1 - cy1) * sv);
    x0 = cx0;
    y0 = cy0;
    x1 = cx1;
    y1 = cy1;
    return true;
}

} // namespace

// =====================================================================================================================
// Frame, memory and batching
// =====================================================================================================================
DrawList::DrawList() { Reset(kUnclipped); }

void DrawList::Reset(const Rect& clipRect) {
    m_lastPrimCount = m_primCount;
    m_primCount = 0;
    m_allocatorFailed = false;
    m_cmds.Clear();
    m_clipStack.Clear();
    m_path.Clear();
    m_chunkOffset = 0;
    if (m_allocator) {
        // The first record asks the allocator for a chunk sized after last frame.
        m_chunkBase = m_write = m_writeEnd = nullptr;
        m_writeBlock = kNoBlock;
    } else {
        m_chunkBase = m_write = m_prims.Data();
        m_writeEnd = m_chunkBase + m_prims.Capacity();
        m_writeBlock = 0;
    }
    m_clipStack.PushBack(clipRect);
    StartCmd(clipRect, kNoTexture);
}

void DrawList::StartCmd(const Rect& clip, TextureId texture) {
    DrawCmd cmd;
    cmd.clipRect = clip;
    cmd.texture = texture;
    cmd.primOffset = WriteOffset();
    cmd.block = m_writeBlock;
    m_cmds.PushBack(cmd);
}

void DrawList::UseTexture(TextureId texture) {
    DrawCmd& cmd = m_cmds.Back();
    if (cmd.texture == kNoTexture) {
        cmd.texture = texture;  // the run had no textured prims yet, so it can adopt this texture
        return;
    }
    const Rect clip = cmd.clipRect;
    StartCmd(clip, texture);
}

void DrawList::NextChunk(uint32_t count) {
    if (m_allocator && !m_allocatorFailed) {
        // Ask for the rest of last frame's volume up front (one chunk per list per frame in the steady state), and
        // grow geometrically past it.
        const uint32_t expected = m_lastPrimCount > m_primCount ? m_lastPrimCount - m_primCount : 0u;
        uint32_t desired = expected + expected / 8 + 64;
        desired = desired > m_primCount / 2 ? desired : m_primCount / 2;
        desired = desired > kMinChunk ? desired : kMinChunk;
        desired = desired > count ? desired : count;
        PrimChunk chunk;
        if (m_allocator->AllocatePrims(count, desired, chunk) && chunk.data && chunk.count >= count &&
            chunk.block != 0 && chunk.block != kNoBlock) {
            if (chunk.block == m_writeBlock && chunk.offset == WriteOffset() && chunk.data == m_write) {
                m_writeEnd = chunk.data + chunk.count;  // right behind the current chunk: keep going
                return;
            }
            m_chunkBase = m_write = chunk.data;
            m_writeEnd = chunk.data + chunk.count;
            m_chunkOffset = chunk.offset;
            m_writeBlock = chunk.block;
        } else {
            // Out of allocator memory: the rest of this frame goes to the list's own memory.
            m_allocatorFailed = true;
            m_prims.Reserve(count > kMinChunk ? count : kMinChunk);
            m_chunkBase = m_write = m_prims.Data();
            m_writeEnd = m_chunkBase + m_prims.Capacity();
            m_chunkOffset = 0;
            m_writeBlock = 0;
        }
        // The current command cannot span two chunks: an empty one moves along, otherwise a new one starts.
        DrawCmd& cmd = m_cmds.Back();
        if (cmd.primCount == 0) {
            cmd.primOffset = WriteOffset();
            cmd.block = m_writeBlock;
        } else {
            const Rect clip = cmd.clipRect;
            const TextureId texture = cmd.texture;
            StartCmd(clip, texture);
        }
        return;
    }
    // Own memory grows in place. Commands hold offsets, so they stay valid when it moves.
    const uint32_t used = uint32_t(m_write - m_prims.Data());
    const uint32_t doubled = m_prims.Capacity() * 2;
    uint32_t capacity = used + count;
    capacity = capacity > doubled ? capacity : doubled;
    m_prims.Reserve(capacity > kMinChunk ? capacity : kMinChunk);
    m_chunkBase = m_prims.Data();
    m_write = m_chunkBase + used;
    m_writeEnd = m_chunkBase + m_prims.Capacity();
}

DZ_FORCEINLINE bool DrawList::Culled(const Rect& b) const {
    const Rect& c = m_cmds.Back().clipRect;
    // Written so that NaN bounds count as culled.
    return !(b.min.x < c.max.x && b.max.x > c.min.x && b.min.y < c.max.y && b.max.y > c.min.y);
}

void DrawList::SetClipRect(const Rect& rect) {
    DrawCmd& current = m_cmds.Back();
    if (current.primCount == 0) {
        // Nothing was drawn under the old clip rect: fold back into the previous command if it matches and the next
        // record would continue it, otherwise reuse this one.
        if (m_cmds.Size() > 1) {
            const DrawCmd& prev = m_cmds[m_cmds.Size() - 2];
            if (prev.clipRect == rect && prev.block == m_writeBlock &&
                prev.primOffset + prev.primCount == WriteOffset()) {
                m_cmds.PopBack();
                return;
            }
        }
        current.clipRect = rect;
        return;
    }
    StartCmd(rect, kNoTexture);
}

void DrawList::PushClipRect(const Rect& rect, bool intersectWithCurrent) {
    const Rect clip = intersectWithCurrent ? rect.Intersect(ClipRect()) : rect;
    m_clipStack.PushBack(clip);
    SetClipRect(clip);
}

void DrawList::PopClipRect() {
    DZ_ASSERT(m_clipStack.Size() > 1 && "PopClipRect without matching PushClipRect");
    if (m_clipStack.Size() <= 1) return;
    m_clipStack.PopBack();
    SetClipRect(m_clipStack.Back());
}

void DrawList::AppendList(const DrawList& other) {
    DZ_ASSERT(other.m_writeBlock == 0 && other.m_primCount == other.OwnPrimCount() &&
              "AppendList takes lists that record into their own memory");
    const uint32_t count = other.OwnPrimCount();
    if (count == 0) return;
    // One contiguous run for all of `other`'s records, then its commands rebased onto it.
    if (size_t(m_writeEnd - m_write) < count) NextChunk(count);
    const uint32_t base = WriteOffset();
    std::memcpy(m_write, other.Prims(), size_t(count) * sizeof(GpuPrim));
    m_write += count;
    m_primCount += count;
    for (const DrawCmd& src : other.m_cmds) {
        if (src.primCount == 0) continue;
        DrawCmd cmd = src;
        cmd.primOffset = base + src.primOffset;
        cmd.block = m_writeBlock;
        DrawCmd& last = m_cmds.Back();
        const bool textureFits =
            last.texture == cmd.texture || last.texture == kNoTexture || cmd.texture == kNoTexture;
        if (last.primCount > 0 && last.block == cmd.block && last.primOffset + last.primCount == cmd.primOffset &&
            last.clipRect == cmd.clipRect && textureFits) {
            last.primCount += cmd.primCount;  // continues the previous run
            if (last.texture == kNoTexture) last.texture = cmd.texture;
        } else if (last.primCount == 0) {
            last = cmd;  // replaces an empty command
        } else {
            m_cmds.PushBack(cmd);
        }
    }
    // Later drawing starts a fresh run under this list's own clip rect.
    StartCmd(m_clipStack.Back(), kNoTexture);
}

DZ_FORCEINLINE void DrawList::PushQuad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color ca, Color cb, Color cc, Color cd) {
    GpuPrim* p = AllocPrims(kNoTexture, 2);
    p->a[0] = a.x;
    p->a[1] = a.y;
    p->a[2] = b.x;
    p->a[3] = b.y;
    p->color = ca;
    p->data[0] = cb;
    p->data[1] = cc;
    p->flags = uint32_t(PrimType::Quad) | kExtended;
    SetContinuation(p + 1, c.x, c.y, d.x, d.y, cd, 0);
}

// =====================================================================================================================
// Rectangles
// =====================================================================================================================
void DrawList::AddRectFilled(const Rect& r, Color color, float rounding) {
    if (ColorAlpha(color) == 0 || Culled(r)) return;
    GpuPrim* p = AllocPrims(kNoTexture, 1);
    SetBox(p, r);
    p->color = color;
    SetRadii(p, rounding, r);
    p->flags = uint32_t(PrimType::Rect);
}

void DrawList::AddRectFilled(const Rect& r, Color color, const CornerRadii& radii) {
    if (ColorAlpha(color) == 0 || Culled(r)) return;
    GpuPrim* p = AllocPrims(kNoTexture, 1);
    SetBox(p, r);
    p->color = color;
    SetRadii(p, radii, r);
    p->flags = uint32_t(PrimType::Rect);
}

void DrawList::AddRect(const Rect& r, Color color, float rounding, float thickness) {
    if (ColorAlpha(color) == 0 || thickness <= 0.0f || Culled(r)) return;
    GpuPrim* p = AllocPrims(kNoTexture, 1);
    SetBox(p, r);
    p->color = color;
    SetRadii(p, rounding, r);
    p->flags = PrimFlags(PrimType::Rect, kOutline, thickness);
}

void DrawList::AddRectGradient(const Rect& r, Color from, Color to, Gradient direction, float rounding) {
    if ((ColorAlpha(from) | ColorAlpha(to)) == 0 || Culled(r)) return;
    if (direction == Gradient::None) {
        AddRectFilled(r, from, rounding);
        return;
    }
    GpuPrim* p = AllocPrims(kNoTexture, 2);
    SetBox(p, r);
    p->color = from;
    SetRadii(p, rounding, r);
    p->flags = uint32_t(PrimType::Rect) | kExtended | GradientFlags(direction);
    SetContinuation(p + 1, 0.0f, 0.0f, 1.0f, 1.0f, to, 0);
}

void DrawList::AddRectFilledMultiColor(const Rect& r, Color tl, Color tr, Color br, Color bl) {
    if ((ColorAlpha(tl) | ColorAlpha(tr) | ColorAlpha(br) | ColorAlpha(bl)) == 0 || Culled(r)) return;
    PushQuad(r.min, {r.max.x, r.min.y}, r.max, {r.min.x, r.max.y}, tl, tr, br, bl);
}

void DrawList::AddRectEx(const Rect& r, const RectStyle& s) {
    const bool hasBorder = s.borderWidth > 0.0f && ColorAlpha(s.borderColor) != 0;
    const bool gradient = s.gradient != Gradient::None;
    const bool hasFill = ColorAlpha(s.fill) != 0 || (gradient && ColorAlpha(s.fillEnd) != 0);
    if ((!hasFill && !hasBorder) || Culled(r)) return;
    if (!hasBorder && !gradient) {
        AddRectFilled(r, s.fill, s.radii);
        return;
    }
    if (!hasFill) {
        GpuPrim* p = AllocPrims(kNoTexture, 1);  // outline only
        SetBox(p, r);
        p->color = s.borderColor;
        SetRadii(p, s.radii, r);
        p->flags = PrimFlags(PrimType::Rect, kOutline, s.borderWidth);
        return;
    }
    GpuPrim* p = AllocPrims(kNoTexture, 2);
    SetBox(p, r);
    p->color = s.fill;
    SetRadii(p, s.radii, r);
    p->flags = PrimFlags(PrimType::Rect, kExtended | GradientFlags(s.gradient), hasBorder ? s.borderWidth : 0.0f);
    SetContinuation(p + 1, 0.0f, 0.0f, 1.0f, 1.0f, gradient ? s.fillEnd : s.fill, hasBorder ? s.borderColor : 0u);
}

void DrawList::AddShadow(const Rect& r, Color color, float blur, float rounding, Vec2 offset, bool cutout) {
    const Rect s = r.Translated(offset);
    if (blur < 0.0f) blur = 0.0f;
    const Rect extent = s.Expanded(blur + kAA);  // everything the shadow can touch
    if (ColorAlpha(color) == 0 || Culled(extent)) return;
    if (!cutout) {
        GpuPrim* p = AllocPrims(kNoTexture, 1);
        SetBox(p, s);
        p->color = color;
        SetRadii(p, rounding, s);
        p->flags = PrimFlags(PrimType::Shadow, 0, blur);
        return;
    }

    // Nothing is drawn where the caster fully covers: inside r, one pixel past its rounded corners. When that hole is
    // big, the shadow is drawn as strips around it so the hole is never shaded at all; the shader cuts the caster's
    // exact shape out of whatever the strips still cover.
    const float radius = Clamp(rounding, 0.0f, MaxRadius(r));
    const Rect hole = r.Expanded(-(radius + kAA)).Intersect(extent);
    Rect strips[4];
    uint32_t stripCount = 0;
    if (hole.Width() >= kMinCutoutInterior && hole.Height() >= kMinCutoutInterior) {
        strips[0] = Rect(extent.min.x, extent.min.y, extent.max.x, hole.min.y);  // top
        strips[1] = Rect(extent.min.x, hole.max.y, extent.max.x, extent.max.y);  // bottom
        strips[2] = Rect(extent.min.x, hole.min.y, hole.min.x, hole.max.y);      // left
        strips[3] = Rect(hole.max.x, hole.min.y, extent.max.x, hole.max.y);      // right
        stripCount = 4;
    } else {
        strips[0] = extent;
        stripCount = 1;
    }
    const uint32_t offsetPacked = PackHalf2(offset.x, offset.y);
    for (uint32_t i = 0; i < stripCount; ++i) {
        const Rect& q = strips[i];
        if (!(q.Width() > 0.0f && q.Height() > 0.0f) || Culled(q)) continue;
        GpuPrim* p = AllocPrims(kNoTexture, 2);
        SetBox(p, s);
        p->color = color;
        SetRadii(p, rounding, s);
        p->flags = PrimFlags(PrimType::Shadow, kExtended | kCutout, blur);
        SetContinuation(p + 1, q.min.x, q.min.y, q.max.x, q.max.y, 0, offsetPacked);
    }
}

// =====================================================================================================================
// Circles and ellipses
// =====================================================================================================================
void DrawList::AddCircleFilled(Vec2 center, float radius, Color color) {
    AddEllipseFilled(center, {radius, radius}, color);
}

void DrawList::AddCircle(Vec2 center, float radius, Color color, float thickness) {
    AddEllipse(center, {radius, radius}, color, thickness);
}

void DrawList::AddEllipseFilled(Vec2 center, Vec2 radii, Color color) {
    const Rect r = Rect::FromCenter(center, radii);
    if (ColorAlpha(color) == 0 || radii.x <= 0.0f || radii.y <= 0.0f || Culled(r)) return;
    GpuPrim* p = AllocPrims(kNoTexture, 1);
    SetBox(p, r);
    p->color = color;
    p->data[0] = 0;
    p->data[1] = 0;
    p->flags = uint32_t(PrimType::Ellipse);
}

void DrawList::AddEllipse(Vec2 center, Vec2 radii, Color color, float thickness) {
    const Rect r = Rect::FromCenter(center, radii);
    if (ColorAlpha(color) == 0 || thickness <= 0.0f || radii.x <= 0.0f || radii.y <= 0.0f || Culled(r)) return;
    GpuPrim* p = AllocPrims(kNoTexture, 1);
    SetBox(p, r);
    p->color = color;
    p->data[0] = 0;
    p->data[1] = 0;
    p->flags = PrimFlags(PrimType::Ellipse, kOutline, thickness);
}

// =====================================================================================================================
// Lines and curves
// =====================================================================================================================
void DrawList::AddLine(Vec2 a, Vec2 b, Color color, float thickness, LineCap cap) {
    if (ColorAlpha(color) == 0 || thickness <= 0.0f) return;
    const float extent = (thickness > 1.0f ? thickness : 1.0f) * 0.5f + kAA;
    if (Culled(Rect(MinV(a, b), MaxV(a, b)).Expanded(extent))) return;
    GpuPrim* p = AllocPrims(kNoTexture, 1);
    p->a[0] = a.x;
    p->a[1] = a.y;
    p->a[2] = b.x;
    p->a[3] = b.y;
    p->color = color;
    p->data[0] = 0;
    p->data[1] = 0;
    p->flags = PrimFlags(PrimType::Line,
                         cap == LineCap::Round ? kCapRound : cap == LineCap::Square ? kCapSquare : 0u, thickness);
}

void DrawList::AddPolyline(const Vec2* points, uint32_t count, Color color, float thickness, bool closed) {
    StrokePolyline(points, count, color, thickness, closed);
}

void DrawList::AddBezierCubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, Color color, float thickness, uint32_t segments) {
    m_path.Clear();
    m_path.PushBack(p0);
    PathBezierCubicTo(p1, p2, p3, segments);
    PathStroke(color, thickness, false);
}

void DrawList::AddBezierQuadratic(Vec2 p0, Vec2 p1, Vec2 p2, Color color, float thickness, uint32_t segments) {
    m_path.Clear();
    m_path.PushBack(p0);
    PathBezierQuadraticTo(p1, p2, segments);
    PathStroke(color, thickness, false);
}

void DrawList::AddArc(Vec2 center, float radius, float angleMin, float angleMax, Color color, float thickness,
                      uint32_t segments) {
    m_path.Clear();
    PathArcTo(center, radius, angleMin, angleMax, segments);
    PathStroke(color, thickness, false);
}

// =====================================================================================================================
// Polygons
// =====================================================================================================================
void DrawList::AddTriangleFilled(Vec2 a, Vec2 b, Vec2 c, Color color) {
    const Vec2 pts[3] = {a, b, c};
    FillPolygon(pts, 3, color, true);
}

void DrawList::AddTriangle(Vec2 a, Vec2 b, Vec2 c, Color color, float thickness) {
    const Vec2 pts[3] = {a, b, c};
    StrokePolyline(pts, 3, color, thickness, true);
}

void DrawList::AddQuadFilled(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color color) {
    const Vec2 pts[4] = {a, b, c, d};
    FillPolygon(pts, 4, color, true);
}

void DrawList::AddQuad(Vec2 a, Vec2 b, Vec2 c, Vec2 d, Color color, float thickness) {
    const Vec2 pts[4] = {a, b, c, d};
    StrokePolyline(pts, 4, color, thickness, true);
}

void DrawList::AddConvexPolyFilled(const Vec2* points, uint32_t count, Color color) {
    FillPolygon(points, count, color, true);
}

void DrawList::AddConcavePolyFilled(const Vec2* points, uint32_t count, Color color) {
    FillPolygon(points, count, color, false);
}

// =====================================================================================================================
// Images
// =====================================================================================================================
void DrawList::AddImage(TextureId texture, const Rect& r, Vec2 uv0, Vec2 uv1, Color tint, float rounding) {
    if (ColorAlpha(tint) == 0 || Culled(r)) return;
    if (texture == kNoTexture) {
        // A plain rect; leaving kTextured off keeps it from sampling whatever texture the batch adopts later.
        AddRectFilled(r, tint, rounding);
        return;
    }
    GpuPrim* p = AllocPrims(texture, 2);
    SetBox(p, r);
    p->color = tint;
    SetRadii(p, rounding, r);
    p->flags = uint32_t(PrimType::Rect) | kExtended | kTextured;
    SetContinuation(p + 1, uv0.x, uv0.y, uv1.x, uv1.y, tint, 0);
}

// =====================================================================================================================
// Text
// =====================================================================================================================
Vec2 DrawList::AddText(const Font& font, float size, Vec2 pos, Color color, std::string_view text) {
    return EmitText(font, size, pos, text, 0.0f, 1.0f, color, 0, 0.0f, 0.0f, nullptr);
}

void DrawList::AddTextClipped(const Font& font, float size, Vec2 pos, Color color, std::string_view text,
                              const Rect& clip) {
    EmitText(font, size, pos, text, 0.0f, 1.0f, color, 0, 0.0f, 0.0f, &clip);
}

Vec2 DrawList::AddText(const Font& font, float size, Vec2 pos, std::string_view text, const TextStyle& style) {
    if (ColorAlpha(style.shadowColor) != 0) {
        // The shadow's silhouette is the glyph plus its outline plus the spread, all in the shadow color.
        EmitText(font, size, pos + style.shadowOffset, text, style.wrapWidth, style.lineSpacing, style.shadowColor,
                 style.shadowColor, style.outlineWidth + style.shadowSpread, style.shadowSoftness, nullptr);
    }
    return EmitText(font, size, pos, text, style.wrapWidth, style.lineSpacing, style.color, style.outlineColor,
                    style.outlineWidth, style.softness, nullptr);
}

Vec2 DrawList::EmitText(const Font& font, float size, Vec2 pos, std::string_view text, float wrapWidth,
                        float lineSpacing, Color fill, Color outline, float outlineWidth, float softness,
                        const Rect* fineClip) {
    if (text.empty() || !(size > 0.0f)) return {};
    const TextureId texture = font.Texture();
    DZ_ASSERT(texture != kNoTexture && "upload the font atlas and call FontAtlas::SetTexture before drawing text");

    // Screen pixels covered by the distance field's range at this size. Effects must stay inside half of it.
    const FontConfig& config = font.Config();
    const float pxRange = config.distanceRange * size / config.atlasEmSize;
    const float maxEffect = std::fmax(pxRange * 0.5f - 0.5f, 0.0f);
    outlineWidth = Clamp(outlineWidth, 0.0f, maxEffect);
    softness = Clamp(softness, 0.0f, maxEffect - outlineWidth);
    const bool hasOutline = outlineWidth > 0.0f && ColorAlpha(outline) != 0;
    if (ColorAlpha(fill) == 0 && !hasOutline) return font.MeasureText(text, size, wrapWidth, lineSpacing);
    // Plain text takes one record per glyph; outlines and softness need a continuation record.
    const bool extended = hasOutline || softness > 0.0f;
    const uint32_t recordsPerGlyph = extended ? 2u : 1u;
    const uint32_t effects = PackHalf2(hasOutline ? outlineWidth : 0.0f, softness);
    const Color outlineColor = hasOutline ? outline : 0;
    const uint32_t flags = PrimFlags(PrimType::Glyph, kTextured | (extended ? kExtended : 0u), pxRange);

    const float ascent = font.Ascent() * size, descent = font.Descent() * size;
    const float lineAdvance = font.LineHeight() * size * lineSpacing;
    const float startX = std::floor(pos.x + 0.5f);
    float baseline = std::floor(pos.y + ascent + 0.5f);
    // A fine clip rect trims glyphs on the CPU (with their texture coordinates) instead of starting a new command.
    const Rect clip = fineClip ? fineClip->Intersect(m_cmds.Back().clipRect) : m_cmds.Back().clipRect;

    const char* s = text.data();
    const char* end = s + text.size();
    float maxWidth = 0.0f;
    uint32_t lines = 0;
    for (;;) {
        const detail::LineInfo line = detail::NextLine(font, s, end, size, wrapWidth, false);
        // Glyph quads overhang the em box (accents, distance field padding); one em of margin covers it.
        const bool lineVisible = baseline + descent + size > clip.min.y && baseline - ascent - size < clip.max.y;
        // A line has at most one glyph per byte: reserve that much once, write glyphs back to back, return the rest.
        const uint32_t reserved = lineVisible ? uint32_t(line.end - s) * recordsPerGlyph : 0u;
        GpuPrim* const first = reserved ? AllocPrims(texture, reserved) : nullptr;
        GpuPrim* out = first;
        float pen = startX;
        // The glyph loop, compiled twice so plain text pays nothing for fine clipping.
        auto emitGlyphs = [&](auto trim) {
            uint32_t prev = 0;
            for (const char* p = s; p < line.end;) {
                // Clipped text: past the clip rect by an em, nothing more on this line can show (AddTextClipped returns
                // no size, so the rest of the line is not even measured).
                if (decltype(trim)::value && pen - size > clip.max.x) break;
                const uint32_t c = detail::DecodeUtf8(p, line.end);
                if (prev) pen += font.Kerning(prev, c) * size;
                prev = c;
                const Glyph& g = font.GetGlyph(c);
                if (out && g.visible) {
                    float x0 = pen + g.x0 * size, x1 = pen + g.x1 * size;
                    if (x1 > clip.min.x && x0 < clip.max.x) {
                        float y0 = baseline + g.y0 * size, y1 = baseline + g.y1 * size;
                        uint32_t uv0 = g.uvPacked[0], uv1 = g.uvPacked[1];
                        if (!decltype(trim)::value || TrimGlyph(g, clip, x0, y0, x1, y1, uv0, uv1)) {
                            out->a[0] = x0;
                            out->a[1] = y0;
                            out->a[2] = x1;
                            out->a[3] = y1;
                            out->color = fill;
                            out->data[0] = uv0;
                            out->data[1] = uv1;
                            out->flags = flags;
                            if (extended) SetContinuation(out + 1, 0.0f, 0.0f, 0.0f, 0.0f, outlineColor, effects);
                            out += recordsPerGlyph;
                        }
                    }
                }
                pen += g.advance * size;
            }
        };
        if (fineClip) {
            emitGlyphs(std::true_type());
        } else {
            emitGlyphs(std::false_type());
        }
        if (first) ReturnPrims(reserved - uint32_t(out - first));
        maxWidth = std::fmax(maxWidth, wrapWidth > 0.0f ? line.width : pen - startX);
        ++lines;
        baseline += lineAdvance;
        if (line.next >= end && !line.newline) break;
        s = line.next;
    }
    return {maxWidth, float(lines) * lineAdvance};
}

// =====================================================================================================================
// Path API
// =====================================================================================================================
uint32_t DrawList::ArcSegments(float radius, float sweep) const {
    const float tol = CurveTolerance();
    // Largest step whose chord stays within `tol` of the arc.
    const float step = radius > tol ? 2.0f * std::acos(1.0f - tol / radius) : kPi * 0.5f;
    const float n = std::ceil(std::fabs(sweep) / step);
    return uint32_t(Clamp(n, 1.0f, float(kMaxCurveSegments)));
}

void DrawList::PathArcTo(Vec2 center, float radius, float angleMin, float angleMax, uint32_t segments) {
    if (radius <= 0.0f) {
        m_path.PushBack(center);
        return;
    }
    if (segments == 0) segments = ArcSegments(radius, angleMax - angleMin);
    Vec2* out = m_path.Append(segments + 1);
    const float step = (angleMax - angleMin) / float(segments);
    for (uint32_t i = 0; i <= segments; ++i) {
        const float a = angleMin + step * float(i);
        out[i] = {center.x + std::cos(a) * radius, center.y + std::sin(a) * radius};
    }
}

void DrawList::PathBezierCubicTo(Vec2 p1, Vec2 p2, Vec2 p3, uint32_t segments) {
    DZ_ASSERT(!m_path.Empty() && "a curve needs a start point");
    const Vec2 p0 = m_path.Empty() ? p1 : m_path.Back();
    if (segments == 0) {
        // Wang's formula: the uniform segment count that keeps the flattening error within tolerance.
        const float m = std::fmax(Length(p0 - p1 * 2.0f + p2), Length(p1 - p2 * 2.0f + p3));
        const float tol = CurveTolerance();
        segments = uint32_t(Clamp(std::ceil(std::sqrt(0.75f * m / tol)), 1.0f, float(kMaxCurveSegments)));
    }
    Vec2* out = m_path.Append(segments);
    const float inv = 1.0f / float(segments);
    for (uint32_t i = 1; i <= segments; ++i) {
        const float t = float(i) * inv, u = 1.0f - t;
        out[i - 1] = p0 * (u * u * u) + p1 * (3.0f * u * u * t) + p2 * (3.0f * u * t * t) + p3 * (t * t * t);
    }
}

void DrawList::PathBezierQuadraticTo(Vec2 p1, Vec2 p2, uint32_t segments) {
    DZ_ASSERT(!m_path.Empty() && "a curve needs a start point");
    const Vec2 p0 = m_path.Empty() ? p1 : m_path.Back();
    if (segments == 0) {
        const float m = Length(p0 - p1 * 2.0f + p2);
        const float tol = CurveTolerance();
        segments = uint32_t(Clamp(std::ceil(std::sqrt(0.25f * m / tol)), 1.0f, float(kMaxCurveSegments)));
    }
    Vec2* out = m_path.Append(segments);
    const float inv = 1.0f / float(segments);
    for (uint32_t i = 1; i <= segments; ++i) {
        const float t = float(i) * inv, u = 1.0f - t;
        out[i - 1] = p0 * (u * u) + p1 * (2.0f * u * t) + p2 * (t * t);
    }
}

void DrawList::PathStroke(Color color, float thickness, bool closed) {
    StrokePolyline(m_path.Data(), m_path.Size(), color, thickness, closed);
    m_path.Clear();
}

void DrawList::PathFillConvex(Color color) {
    FillPolygon(m_path.Data(), m_path.Size(), color, true);
    m_path.Clear();
}

void DrawList::PathFillConcave(Color color) {
    FillPolygon(m_path.Data(), m_path.Size(), color, false);
    m_path.Clear();
}

// =====================================================================================================================
// Tessellation
// =====================================================================================================================

// Copies `points` into m_scratch without consecutive duplicates (and without a duplicated closing point when
// `closed`), followed by room for `extraArrays` more arrays of `count` entries each: array k starts at
// m_scratch.Data() + k * count. Returns the number of points kept.
uint32_t DrawList::PreparePolygon(const Vec2* points, uint32_t count, bool closed, uint32_t extraArrays) {
    m_scratch.Resize(count * (1 + extraArrays));
    Vec2* clean = m_scratch.Data();
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (n == 0 || LengthSq(points[i] - clean[n - 1]) > kDuplicateDistSq) clean[n++] = points[i];
    }
    if (closed) {
        while (n > 1 && LengthSq(clean[n - 1] - clean[0]) <= kDuplicateDistSq) --n;
    }
    return n;
}

void DrawList::StrokePolyline(const Vec2* points, uint32_t count, Color color, float thickness, bool closed) {
    if (count < 2 || ColorAlpha(color) == 0 || !(thickness > 0.0f)) return;
    const uint32_t n = PreparePolygon(points, count, closed, 0);
    if (n < 2) return;
    if (n < 3) closed = false;

    // One Stroke record per segment, linked to the records of its neighbours. The vertex shader builds the miter at
    // each point from the neighbour's far point; both segments at a point compute it from the same three points, so
    // joins meet exactly. The pixel shader anti-aliases across the stroke and across an open polyline's two ends.
    // Strokes thinner than one pixel are drawn one pixel wide with proportionally lower alpha, like lines.
    const float aa = FringeWidth();
    const float reach = ((thickness > aa ? thickness : aa) * 0.5f + aa) * kMaxMiter;  // farthest a corner can go
    const Vec2* pts = m_scratch.Data();
    const Rect bounds = BoundsOf(pts, n).Expanded(reach);
    if (Culled(bounds)) return;

    const uint32_t segmentCount = closed ? n : n - 1;
    const uint32_t flags = PrimFlags(PrimType::Stroke, 0, thickness);
    const Rect clip = m_cmds.Back().clipRect;
    GpuPrim* const first = AllocPrims(kNoTexture, segmentCount);
    auto write = [&](GpuPrim* out, uint32_t i, int32_t prevLink, int32_t nextLink) {
        const Vec2 a = pts[i], b = pts[i + 1 == n ? 0 : i + 1];
        out->a[0] = a.x;
        out->a[1] = a.y;
        out->a[2] = b.x;
        out->a[3] = b.y;
        out->color = color;
        out->data[0] = uint32_t(prevLink);
        out->data[1] = uint32_t(nextLink);
        uint32_t f = flags;
        if (!closed && i == 0) f |= kStrokeFadeStart;
        if (!closed && i + 1 == segmentCount) f |= kStrokeFadeEnd;
        out->flags = f;
    };

    const int32_t last = int32_t(segmentCount) - 1;
    if (bounds.min.x >= clip.min.x && bounds.min.y >= clip.min.y && bounds.max.x <= clip.max.x &&
        bounds.max.y <= clip.max.y) {
        // Entirely inside the clip rect (the usual case): every segment, each linked to its neighbours.
        for (uint32_t i = 0; i < segmentCount; ++i) {
            const int32_t prevLink = i > 0 ? -1 : (closed ? last : 0);
            const int32_t nextLink = int32_t(i) < last ? 1 : (closed ? -last : 0);
            write(first + i, i, prevLink, nextLink);
        }
        return;
    }

    // Partly visible: segments outside the clip rect are skipped, and their neighbours unlinked from them (the point
    // they shared lies outside the clip, so its corner cannot be seen).
    m_scratchIndices.Resize(segmentCount);
    uint32_t* visible = m_scratchIndices.Data();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < segmentCount; ++i) {
        const Vec2 a = pts[i], b = pts[i + 1 == n ? 0 : i + 1];
        visible[i] = Culled(Rect(MinV(a, b), MaxV(a, b)).Expanded(reach)) ? 0u : 1u;
        kept += visible[i];
    }
    GpuPrim* out = first;
    const int32_t wrap = int32_t(kept) - 1;  // from the first kept record to the last
    for (uint32_t i = 0; i < segmentCount; ++i) {
        if (!visible[i]) continue;
        const int32_t prevLink = i > 0 ? (visible[i - 1] ? -1 : 0) : (closed && visible[last] ? wrap : 0);
        const int32_t nextLink =
            int32_t(i) < last ? (visible[i + 1] ? 1 : 0) : (closed && visible[0] ? -wrap : 0);
        write(out++, i, prevLink, nextLink);
    }
    ReturnPrims(segmentCount - kept);
}

void DrawList::FillPolygon(const Vec2* points, uint32_t count, Color color, bool convex) {
    if (count < 3 || ColorAlpha(color) == 0) return;
    const uint32_t n = PreparePolygon(points, count, true, 0);
    if (n < 3) return;
    const Vec2* pts = m_scratch.Data();
    // The anti-aliasing band reaches half a pixel past the edges, further at sharp corners (miters up to kMaxMiter).
    if (Culled(BoundsOf(pts, n).Expanded(FringeWidth() * 0.5f * kMaxMiter))) return;

    const float area = SignedArea2(pts, n);
    if (std::fabs(area) < 1e-6f) return;
    const uint32_t flags = uint32_t(PrimType::Polygon) | (area > 0.0f ? 0u : kPolyCcw);
    // Point indices are stored as float values (exact below 2^24), not raw bits: small integers' bits are denormal
    // floats, which D3D11 drivers may flush to zero.
    auto setIndices = [](GpuPrim* p, uint32_t i, uint32_t j, uint32_t k, uint32_t l) {
        p->a[0] = float(i);
        p->a[1] = float(j);
        p->a[2] = float(k);
        p->a[3] = float(l);
    };

    if (n == 3) {
        // A lone triangle: one record, every edge anti-aliased by the shader.
        GpuPrim* p = AllocPrims(kNoTexture, 1);
        SetBox(p, Rect(pts[0], pts[1]));
        p->color = color;
        std::memcpy(&p->data[0], &pts[2].x, sizeof(float));
        std::memcpy(&p->data[1], &pts[2].y, sizeof(float));
        p->flags = flags | kPolyTriangle;
        return;
    }

    // One record per edge for the anti-aliasing band, then the interior in records of two triangles: a fan from point
    // 0 for convex polygons, ear clipping otherwise. The shader finds each point's neighbours through the edge records
    // and moves it along its miter (out for the band's outer side, in for the band's inner side and the interior).
    if (!convex) Triangulate(pts, n, area > 0.0f ? 1.0f : -1.0f, m_scratchIndices, m_triangles);
    const uint32_t interior = convex ? (n - 1) / 2 : m_triangles.Size() / 3;
    GpuPrim* const first = AllocPrims(kNoTexture, n + interior);
    for (uint32_t k = 0; k < n; ++k) {
        GpuPrim* p = first + k;
        SetBox(p, Rect(pts[k], pts[k + 1 == n ? 0 : k + 1]));
        p->color = color;
        p->data[0] = k;
        p->data[1] = n;
        p->flags = flags | kPolyEdge;
    }
    GpuPrim* p = first + n;
    for (uint32_t q = 0; q < interior; ++q, ++p) {
        if (convex) {
            const uint32_t i = 1 + 2 * q;
            setIndices(p, 0, i, i + 1, i + 2 < n ? i + 2 : i + 1);  // the last record may hold only one triangle
        } else {
            const uint32_t* t = m_triangles.Data() + 3 * q;
            setIndices(p, t[0], t[1], t[2], t[2]);
        }
        p->color = color;
        p->data[0] = uint32_t(-int32_t(n + q));  // link back to edge 0
        p->data[1] = n;
        p->flags = flags | kPolyInterior;
    }
}

} // namespace drizzy
