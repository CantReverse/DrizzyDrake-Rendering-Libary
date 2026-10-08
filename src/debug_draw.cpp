#include "drizzy/debug_draw.h"

#include <cmath>

namespace drizzy {
namespace {

constexpr float kNearEps = 1e-4f;  // clip-space w threshold for "in front of the camera"

Vec4 Lerp4(const Vec4& a, const Vec4& b, float t) {
    return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t};
}

} // namespace

void DebugDraw3D::Begin(const Mat4& viewProj, const Rect& viewport, DrawList* drawList, bool ndcYUp) {
    m_viewProj = viewProj;
    m_viewport = viewport;
    m_dl = drawList;
    m_ndcYSign = ndcYUp ? -1.0f : 1.0f;
}

Vec2 DebugDraw3D::ClipToScreen(const Vec4& clip) const {
    const float inv = 1.0f / clip.w;
    const float nx = clip.x * inv, ny = clip.y * inv;
    return {m_viewport.min.x + (nx * 0.5f + 0.5f) * m_viewport.Width(),
            m_viewport.min.y + (0.5f + m_ndcYSign * ny * 0.5f) * m_viewport.Height()};
}

bool DebugDraw3D::WorldToScreen(Vec3 world, Vec2& out) const {
    const Vec4 clip = m_viewProj * Vec4(world, 1.0f);
    if (clip.w <= kNearEps) return false;
    out = ClipToScreen(clip);
    return true;
}

void DebugDraw3D::Line(Vec3 a, Vec3 b, Color color, float thickness) {
    if (!m_dl) return;
    Vec4 ca = m_viewProj * Vec4(a, 1.0f);
    Vec4 cb = m_viewProj * Vec4(b, 1.0f);
    if (ca.w <= kNearEps && cb.w <= kNearEps) return;  // both behind the camera
    if (ca.w <= kNearEps) ca = Lerp4(ca, cb, (kNearEps - ca.w) / (cb.w - ca.w));
    else if (cb.w <= kNearEps) cb = Lerp4(cb, ca, (kNearEps - cb.w) / (ca.w - cb.w));
    m_dl->AddLine(ClipToScreen(ca), ClipToScreen(cb), color, thickness);
}

void DebugDraw3D::Arrow(Vec3 from, Vec3 to, Color color, float thickness, float headSize) {
    Line(from, to, color, thickness);
    const Vec3 dir = Normalize(to - from);
    if (Dot(dir, dir) <= 0.0f) return;
    if (headSize <= 0.0f) headSize = Length(to - from) * 0.15f;
    // Build two axes perpendicular to the arrow to splay the head.
    Vec3 up = std::fabs(dir.y) < 0.95f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    const Vec3 side = Normalize(Cross(dir, up));
    up = Cross(side, dir);
    const Vec3 base = to - dir * headSize;
    Line(to, base + side * (headSize * 0.5f), color, thickness);
    Line(to, base - side * (headSize * 0.5f), color, thickness);
    Line(to, base + up * (headSize * 0.5f), color, thickness);
    Line(to, base - up * (headSize * 0.5f), color, thickness);
}

void DebugDraw3D::Box(Vec3 mn, Vec3 mx, Color color, float thickness) {
    const Vec3 c[8] = {{mn.x, mn.y, mn.z}, {mx.x, mn.y, mn.z}, {mx.x, mx.y, mn.z}, {mn.x, mx.y, mn.z},
                       {mn.x, mn.y, mx.z}, {mx.x, mn.y, mx.z}, {mx.x, mx.y, mx.z}, {mn.x, mx.y, mx.z}};
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                     {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) Line(c[e[0]], c[e[1]], color, thickness);
}

void DebugDraw3D::BoxOriented(const Mat4& model, Vec3 h, Color color, float thickness) {
    Vec3 c[8];
    const Vec3 local[8] = {{-h.x, -h.y, -h.z}, {h.x, -h.y, -h.z}, {h.x, h.y, -h.z}, {-h.x, h.y, -h.z},
                           {-h.x, -h.y, h.z},  {h.x, -h.y, h.z},  {h.x, h.y, h.z},  {-h.x, h.y, h.z}};
    for (int i = 0; i < 8; ++i) {
        const Vec4 p = model * Vec4(local[i], 1.0f);
        c[i] = {p.x, p.y, p.z};
    }
    static const int edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {4, 5}, {5, 6},
                                     {6, 7}, {7, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& e : edges) Line(c[e[0]], c[e[1]], color, thickness);
}

void DebugDraw3D::Circle(Vec3 center, Vec3 axis, float radius, Color color, int segments, float thickness) {
    axis = Normalize(axis);
    if (Dot(axis, axis) <= 0.0f) return;
    Vec3 u = std::fabs(axis.y) < 0.95f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    u = Normalize(Cross(axis, u));
    const Vec3 v = Cross(axis, u);
    if (segments < 3) segments = 3;
    Vec3 prev = center + u * radius;
    for (int i = 1; i <= segments; ++i) {
        const float a = float(i) / float(segments) * 2.0f * kPi;
        const Vec3 p = center + (u * std::cos(a) + v * std::sin(a)) * radius;
        Line(prev, p, color, thickness);
        prev = p;
    }
}

void DebugDraw3D::Sphere(Vec3 center, float radius, Color color, int segments, float thickness) {
    Circle(center, {1, 0, 0}, radius, color, segments, thickness);
    Circle(center, {0, 1, 0}, radius, color, segments, thickness);
    Circle(center, {0, 0, 1}, radius, color, segments, thickness);
}

void DebugDraw3D::AxisGizmo(const Mat4& model, float length, float thickness) {
    const Vec4 o = model * Vec4(0, 0, 0, 1);
    const Vec3 origin{o.x, o.y, o.z};
    auto axisTip = [&](Vec3 dir) {
        const Vec4 p = model * Vec4(dir * length, 1.0f);
        return Vec3{p.x, p.y, p.z};
    };
    Line(origin, axisTip({1, 0, 0}), Hex(0xFF4040), thickness);
    Line(origin, axisTip({0, 1, 0}), Hex(0x40FF40), thickness);
    Line(origin, axisTip({0, 0, 1}), Hex(0x4080FF), thickness);
}

void DebugDraw3D::Point(Vec3 world, float screenRadius, Color color) {
    Vec2 s;
    if (m_dl && WorldToScreen(world, s)) m_dl->AddCircleFilled(s, screenRadius, color);
}

void DebugDraw3D::Text(Vec3 world, const Font& font, float size, Color color, std::string_view text, Vec2 align) {
    Vec2 s;
    if (!m_dl || !WorldToScreen(world, s)) return;
    const Vec2 extent = font.MeasureText(text, size);
    m_dl->AddText(font, size, {s.x - extent.x * align.x, s.y - extent.y * align.y}, color, text);
}

} // namespace drizzy
