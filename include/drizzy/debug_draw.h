// drizzy_renderer - world-space 3D debug drawing.
//
// Projects 3D world positions to the screen with a view*projection matrix you supply (from the game), and emits lines,
// boxes, spheres, arrows and text into a DrawList - handy in a mod overlay for visualizing positions, bounds, paths or
// "ESP"-style markers. Lines are clipped against the near plane so geometry behind the camera does not streak.
//
//   DebugDraw3D dd;
//   dd.Begin(viewProj, {{0,0}, displaySize}, &ui.ForegroundDrawList());
//   dd.Box(min, max, drizzy::Hex(0x00FF88));
//   dd.Text(headPos, *font, 14.0f, drizzy::colors::White, "Enemy");
#pragma once

#include "drizzy/draw_list.h"
#include "drizzy/font.h"

#include <string_view>

namespace drizzy {

class DebugDraw3D {
public:
    // viewProj: world -> clip (view * projection), row-major (Mat4::FromColumnMajor for transposed engines).
    // viewport: the screen rectangle to map NDC into (usually the whole display). ndcYUp matches D3D/Unity/Unreal clip
    // space (y points up in NDC); pass false only for a GL-style projection.
    void Begin(const Mat4& viewProj, const Rect& viewport, DrawList* drawList, bool ndcYUp = true);

    // Projects a world point. Returns false when it is behind the camera (near plane); `out` is then unset.
    bool WorldToScreen(Vec3 world, Vec2& out) const;

    void Line(Vec3 a, Vec3 b, Color color, float thickness = 1.5f);
    void Arrow(Vec3 from, Vec3 to, Color color, float thickness = 1.5f, float headSize = 0.0f);
    // Axis-aligned box from its two opposite corners.
    void Box(Vec3 min, Vec3 max, Color color, float thickness = 1.5f);
    // Oriented box: `model` (world transform, row-major) applied to a box of the given half-extents centered at origin.
    void BoxOriented(const Mat4& model, Vec3 halfExtents, Color color, float thickness = 1.5f);
    // Great-circle wireframe sphere (three rings).
    void Sphere(Vec3 center, float radius, Color color, int segments = 24, float thickness = 1.5f);
    // Circle in the plane whose normal is `axis`.
    void Circle(Vec3 center, Vec3 axis, float radius, Color color, int segments = 24, float thickness = 1.5f);
    // Three short axis lines (X red, Y green, Z blue) of the given length, oriented by `model`.
    void AxisGizmo(const Mat4& model, float length, float thickness = 2.0f);
    // A filled screen-space dot at the world position (if in front of the camera).
    void Point(Vec3 world, float screenRadius, Color color);
    // Text billboarded at a world position; `size` is in screen pixels. align is 0..1 within the text's own box.
    void Text(Vec3 world, const Font& font, float size, Color color, std::string_view text, Vec2 align = {0.5f, 0.0f});

    Vec2 ViewportSize() const { return m_viewport.Size(); }

private:
    Vec2 ClipToScreen(const Vec4& clip) const;

    Mat4 m_viewProj;
    Rect m_viewport;
    DrawList* m_dl = nullptr;
    float m_ndcYSign = -1.0f;  // screen y = center - ndc.y * sign (so +1 NDC maps to the top when ndcYUp)
};

} // namespace drizzy
