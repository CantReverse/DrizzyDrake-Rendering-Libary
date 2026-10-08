#include "scenes.h"

#include "drizzy/debug_draw.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <random>
#include <string_view>

using namespace drizzy;

namespace sandbox {
namespace {

constexpr Color kPanel = Hex(0x262631);
constexpr Color kCaption = Rgba(255, 255, 255, 120);

Color Hsv(float h, float s, float v, float a = 1.0f) {
    h -= std::floor(h);
    const float r = Clamp(std::fabs(h * 6.0f - 3.0f) - 1.0f, 0.0f, 1.0f);
    const float g = Clamp(2.0f - std::fabs(h * 6.0f - 2.0f), 0.0f, 1.0f);
    const float b = Clamp(2.0f - std::fabs(h * 6.0f - 4.0f), 0.0f, 1.0f);
    return RgbaF(v * Lerp(1.0f, r, s), v * Lerp(1.0f, g, s), v * Lerp(1.0f, b, s), a);
}

Vec2 Polar(float angle, float radius) { return {std::cos(angle) * radius, std::sin(angle) * radius}; }

Rect Cell(int col, int row) {
    const float x = 40.0f + float(col) * 152.0f, y = 44.0f + float(row) * 164.0f;
    return {x, y, x + 140.0f, y + 140.0f};
}

constexpr const char* kCaptions[4][8] = {
    {"AddRectFilled", "rounded", "per-corner radii", "AddRect outlines", "gradient", "gradient to clear",
     "shadow + border", "4-corner colors"},
    {"AddCircleFilled", "AddCircle", "AddEllipseFilled", "AddEllipse", "translucent blend", "tiny circles",
     "glow (AddShadow)", "animated"},
    {"line widths", "butt / round / square", "starburst", "AddPolyline", "AddBezierCubic", "AddArc", "AddTriangle",
     "quadratic + wave"},
    {"AddTriangleFilled", "convex polygon", "concave polygon", "AddImage", "rounded + tinted", "uv zoom + circle",
     "PushClipRect", "progress ring"},
};

} // namespace

std::vector<uint32_t> MakeCheckerPixels(uint32_t size) {
    std::vector<uint32_t> pixels(size_t(size) * size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t r = x * 255 / (size - 1), g = y * 255 / (size - 1);
            const bool odd = ((x / 8) + (y / 8)) & 1;
            pixels[size_t(y) * size + x] = odd ? Rgba(r, g, 230) : Rgba(r / 3, g / 3, 70);
        }
    }
    return pixels;
}

// =====================================================================================================================
// Shapes
// =====================================================================================================================
void BuildShapesScene(DrawList& dl, float time, const SceneAssets& assets) {
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 8; ++col) {
            const Rect cell = Cell(col, row);
            dl.AddRectFilled(cell, kPanel, 10.0f);
            if (assets.font) dl.AddText(*assets.font, 12.0f, {cell.min.x + 2.0f, cell.min.y - 18.0f}, kCaption,
                                        kCaptions[row][col]);
        }
    }

    // ---- Row 0: rectangles ------------------------------------------------------------------------------------------
    dl.AddRectFilled(Cell(0, 0).Expanded(-26.0f), Hex(0xF28C28));
    dl.AddRectFilled(Cell(1, 0).Expanded(-26.0f), Hex(0x2EC4B6), 22.0f);
    dl.AddRectFilled(Cell(2, 0).Expanded(-26.0f), Hex(0x9B5DE5), CornerRadii(0.0f, 36.0f, 0.0f, 36.0f));
    dl.AddRect(Cell(3, 0).Expanded(-20.0f), colors::White, 0.0f, 1.0f);
    dl.AddRect(Cell(3, 0).Expanded(-34.0f), Hex(0xF15BB5), 14.0f, 3.0f);
    dl.AddRectGradient(Cell(4, 0).Expanded(-26.0f), Hex(0xF15BB5), Hex(0x00BBF9), Gradient::Horizontal, 18.0f);
    dl.AddRectGradient(Cell(5, 0).Expanded(-26.0f), Hex(0xFEE440), Hex(0xFEE440, 0), Gradient::Vertical);
    {
        const Rect card = Cell(6, 0).Expanded(-28.0f);
        dl.AddShadow(card, Rgba(0, 0, 0, 220), 12.0f, 14.0f, {0.0f, 6.0f});
        RectStyle style;
        style.fill = Hex(0x3A3A4A);
        style.radii = 14.0f;
        style.borderWidth = 2.0f;
        style.borderColor = Hex(0x00F5D4);
        dl.AddRectEx(card, style);
    }
    dl.AddRectFilledMultiColor(Cell(7, 0).Expanded(-26.0f), Hex(0xFF595E), Hex(0xFFCA3A), Hex(0x8AC926),
                               Hex(0x1982C4));

    // ---- Row 1: circles and ellipses --------------------------------------------------------------------------------
    dl.AddCircleFilled(Cell(0, 1).Center(), 48.0f, Hex(0x00BBF9));
    {
        const Vec2 c = Cell(1, 1).Center();
        dl.AddCircle(c, 50.0f, colors::White, 1.0f);
        dl.AddCircle(c, 38.0f, Hex(0xFEE440), 4.0f);
        dl.AddCircle(c, 22.0f, Hex(0xF15BB5), 9.0f);
    }
    dl.AddEllipseFilled(Cell(2, 1).Center(), {56.0f, 30.0f}, Hex(0x9B5DE5));
    dl.AddEllipse(Cell(3, 1).Center(), {34.0f, 54.0f}, Hex(0x2EC4B6), 3.0f);
    {
        const Vec2 c = Cell(4, 1).Center();
        dl.AddCircleFilled(c + Vec2(-15.0f, -10.0f), 32.0f, Rgba(255, 70, 70, 150));
        dl.AddCircleFilled(c + Vec2(15.0f, -10.0f), 32.0f, Rgba(70, 255, 70, 150));
        dl.AddCircleFilled(c + Vec2(0.0f, 16.0f), 32.0f, Rgba(70, 120, 255, 150));
    }
    {
        const Rect cell = Cell(5, 1);
        float x = cell.min.x + 16.0f;
        for (float r : {1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 6.0f, 8.0f}) {
            dl.AddCircleFilled({x + r, cell.min.y + 45.0f}, r, colors::White);
            dl.AddCircle({x + r, cell.min.y + 95.0f}, r, Hex(0xFEE440), 1.0f);
            x += 2.0f * r + 6.0f;
        }
    }
    {
        const Rect card = Cell(6, 1).Expanded(-38.0f);
        dl.AddShadow(card, Hex(0x00F5D4, 230), 18.0f, 8.0f);
        dl.AddRectFilled(card, Hex(0xE8E8F0), 8.0f);
    }
    {
        const Vec2 c = Cell(7, 1).Center();
        for (int i = 0; i < 12; ++i) {
            const float a = time * 2.0f + float(i) * (2.0f * kPi / 12.0f);
            dl.AddCircleFilled(c + Polar(a, 40.0f), 3.0f + float(i) * 0.5f,
                               ScaleAlpha(Hex(0xFF8C42), float(i + 1) / 12.0f));
        }
    }

    // ---- Row 2: lines and curves ------------------------------------------------------------------------------------
    {
        const Rect cell = Cell(0, 2);
        float y = cell.min.y + 18.5f;
        for (float w : {0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 5.0f, 8.0f}) {
            dl.AddLine({cell.min.x + 16.0f, y}, {cell.max.x - 16.0f, y}, colors::White, w);
            y += 16.0f;
        }
    }
    {
        const Rect cell = Cell(1, 2);
        const float top = cell.min.y + 34.0f, bottom = cell.max.y - 34.0f;
        const LineCap caps[] = {LineCap::Butt, LineCap::Round, LineCap::Square};
        for (int i = 0; i < 3; ++i) {
            const float x = cell.min.x + 35.0f + float(i) * 35.0f;
            dl.AddLine({x, top}, {x, bottom}, Hex(0x00BBF9), 14.0f, caps[i]);
        }
        // Guides at the segment end points show how far each cap extends.
        dl.AddLine({cell.min.x + 8.0f, top}, {cell.max.x - 8.0f, top}, Rgba(255, 80, 80, 220));
        dl.AddLine({cell.min.x + 8.0f, bottom}, {cell.max.x - 8.0f, bottom}, Rgba(255, 80, 80, 220));
    }
    {
        const Vec2 c = Cell(2, 2).Center();
        for (int i = 0; i < 24; ++i) {
            const float a = float(i) * (2.0f * kPi / 24.0f);
            dl.AddLine(c + Polar(a, 10.0f), c + Polar(a, 54.0f), Hsv(float(i) / 24.0f, 0.7f, 1.0f), 1.25f,
                       LineCap::Round);
        }
    }
    {
        const Rect cell = Cell(3, 2);
        Vec2 pts[6];
        for (int i = 0; i < 6; ++i) {
            pts[i] = {cell.min.x + 18.0f + float(i) * 20.8f, (i & 1) ? cell.max.y - 36.0f : cell.min.y + 36.0f};
        }
        dl.AddPolyline(pts, 6, Hex(0x8AC926), 6.0f);
    }
    {
        const Rect cell = Cell(4, 2);
        const Vec2 p0(cell.min.x + 16.0f, cell.max.y - 24.0f), p1(cell.min.x + 30.0f, cell.min.y + 10.0f);
        const Vec2 p2(cell.max.x - 30.0f, cell.max.y - 10.0f), p3(cell.max.x - 16.0f, cell.min.y + 24.0f);
        dl.AddLine(p0, p1, Rgba(255, 255, 255, 90));
        dl.AddLine(p2, p3, Rgba(255, 255, 255, 90));
        dl.AddBezierCubic(p0, p1, p2, p3, Hex(0xF15BB5), 3.0f);
        for (Vec2 p : {p0, p1, p2, p3}) dl.AddCircleFilled(p, 3.5f, colors::White);
    }
    {
        const Vec2 c = Cell(5, 2).Center();
        dl.AddArc(c, 44.0f, kPi * 0.75f, kPi * 2.25f, Hex(0xFEE440), 8.0f);
        dl.AddArc(c, 28.0f, 0.0f, kPi * 1.25f, Hex(0x00F5D4), 2.0f);
    }
    {
        const Rect cell = Cell(6, 2);
        const Vec2 a(cell.Center().x, cell.min.y + 22.0f), b(cell.max.x - 20.0f, cell.max.y - 26.0f),
            c(cell.min.x + 20.0f, cell.max.y - 26.0f);
        dl.AddTriangle(a, b, c, Hex(0xFF595E), 5.0f);
        const Vec2 g = (a + b + c) / 3.0f;
        dl.AddTriangle(g + (a - g) * 0.45f, g + (b - g) * 0.45f, g + (c - g) * 0.45f, colors::White, 1.0f);
    }
    {
        const Rect cell = Cell(7, 2);
        Vec2 wave[48];
        for (int i = 0; i < 48; ++i) {
            wave[i] = {cell.min.x + 12.0f + float(i) * (116.0f / 47.0f),
                       cell.Center().y + 26.0f + std::sin(float(i) * 0.4f + time * 3.0f) * 12.0f};
        }
        dl.AddPolyline(wave, 48, Hex(0x00BBF9), 1.0f);
        dl.AddBezierQuadratic({cell.min.x + 14.0f, cell.Center().y - 6.0f}, {cell.Center().x, cell.min.y - 30.0f},
                              {cell.max.x - 14.0f, cell.Center().y - 6.0f}, Hex(0xFFCA3A), 4.0f);
    }

    // ---- Row 3: polygons, images, clipping --------------------------------------------------------------------------
    {
        const Rect cell = Cell(0, 3);
        dl.AddTriangleFilled({cell.Center().x, cell.min.y + 22.0f}, {cell.max.x - 20.0f, cell.max.y - 24.0f},
                             {cell.min.x + 20.0f, cell.max.y - 24.0f}, Hex(0xFF8C42));
    }
    {
        const Vec2 c = Cell(1, 3).Center();
        Vec2 hexagon[6];
        for (int i = 0; i < 6; ++i) hexagon[i] = c + Polar(float(i) * kPi / 3.0f + time * 0.5f, 50.0f);
        dl.AddConvexPolyFilled(hexagon, 6, Hex(0x2EC4B6));
    }
    {
        const Vec2 c = Cell(2, 3).Center();
        Vec2 star[10];
        for (int i = 0; i < 10; ++i) star[i] = c + Polar(-kPi * 0.5f + float(i) * kPi / 5.0f, (i & 1) ? 21.0f : 52.0f);
        dl.AddConcavePolyFilled(star, 10, Hex(0xFEE440));
    }
    dl.AddImage(assets.checker, Cell(3, 3).Expanded(-20.0f));
    dl.AddImage(assets.checker, Cell(4, 3).Expanded(-20.0f), {0.0f, 0.0f}, {1.0f, 1.0f}, Hex(0xFFB0E0), 24.0f);
    dl.AddImage(assets.checker, Cell(5, 3).Expanded(-20.0f), {0.25f, 0.25f}, {0.75f, 0.75f}, colors::White, 50.0f);
    {
        const Rect clip = Cell(6, 3).Expanded(-24.0f);
        dl.AddRect(clip.Expanded(1.0f), Rgba(255, 255, 255, 90));
        dl.PushClipRect(clip);
        dl.AddCircleFilled(clip.max, 60.0f, Hex(0x9B5DE5));
        for (int i = 0; i < 8; ++i) {
            dl.AddLine({clip.min.x - 20.0f, clip.min.y + float(i) * 14.0f},
                       {clip.max.x + 20.0f, clip.min.y + float(i) * 14.0f + 40.0f}, Hex(0x00F5D4), 3.0f);
        }
        dl.PopClipRect();
    }
    {
        const Vec2 c = Cell(7, 3).Center();
        dl.AddCircle(c, 46.0f, Rgba(255, 255, 255, 40), 10.0f);
        dl.AddArc(c, 41.0f, -kPi * 0.5f, -kPi * 0.5f + 0.68f * 2.0f * kPi, Hex(0x8AC926), 10.0f);
        dl.AddCircleFilled(c, 22.0f, Hex(0x8AC926, 60));
        if (assets.font) {
            const std::string_view label = "68%";
            const Vec2 size = assets.font->MeasureText(label, 15.0f);
            dl.AddText(*assets.font, 15.0f, c - size * 0.5f, colors::White, label);
        }
    }
}

// =====================================================================================================================
// Text
// =====================================================================================================================
void BuildTextScene(DrawList& dl, float time, const SceneAssets& assets) {
    if (!assets.font) return;
    const Font& font = *assets.font;

    TextStyle title;
    title.shadowColor = Rgba(0, 0, 0, 200);
    title.shadowOffset = {2.0f, 3.0f};
    title.shadowSoftness = 2.0f;
    dl.AddText(font, 34.0f, {40.0f, 22.0f}, "Text", title);
    dl.AddText(font, 14.0f, {40.0f, 68.0f}, kCaption,
               "Inter Regular  -  one MTSDF atlas for every size  -  glyphs batch with shapes in the same draw calls");

    // ---- Left: size ladder, wrapping, measuring ---------------------------------------------------------------------
    const Rect left(40.0f, 100.0f, 760.0f, 680.0f);
    dl.AddRectFilled(left, kPanel, 12.0f);
    dl.PushClipRect(left.Expanded(-8.0f));
    float y = left.min.y + 18.0f;
    for (float size : {10.0f, 12.0f, 14.0f, 16.0f, 20.0f, 24.0f, 32.0f}) {
        char label[16];
        std::snprintf(label, sizeof(label), "%gpx", size);
        dl.AddText(font, 12.0f, {left.min.x + 16.0f, y + (size - 12.0f) * 0.7f}, kCaption, label);
        y += dl.AddText(font, size, {left.min.x + 72.0f, y}, colors::White,
                        "Sphinx of black quartz, judge my vow. 0123456789").y + 6.0f;
    }

    // Word wrap inside a box sized by MeasureText.
    const char* paragraph =
        "Word wrapping keeps whole words together and breaks at spaces. Latin-1 is included by default: "
        "caf\xC3\xA9, na\xC3\xAFve, Gr\xC3\xB6\xC3\x9F" "e, S\xC3\xA3o Paulo, \xC2\xBFQu\xC3\xA9 tal? The box behind "
        "this paragraph was sized with Font::MeasureText.";
    const float wrap = 330.0f;
    const Vec2 paraPos(left.min.x + 24.0f, y + 26.0f);
    const Vec2 paraSize = font.MeasureText(paragraph, 15.0f, wrap);
    dl.AddRectFilled(Rect(paraPos - Vec2(12.0f, 10.0f), paraPos + paraSize + Vec2(12.0f, 10.0f)), Hex(0x323244), 8.0f);
    TextStyle wrapped;
    wrapped.color = Hex(0xE8E8F0);
    wrapped.wrapWidth = wrap;
    dl.AddText(font, 15.0f, paraPos, paragraph, wrapped);

    // Alignment from measured sizes.
    const Rect box(left.min.x + 400.0f, y + 16.0f, left.max.x - 24.0f, left.max.y - 20.0f);
    dl.AddRect(box, Rgba(255, 255, 255, 60), 8.0f);
    const char* lines[] = {"Left aligned", "Centered", "Right aligned"};
    for (int i = 0; i < 3; ++i) {
        const Vec2 size = font.MeasureText(lines[i], 18.0f);
        const float x = i == 0 ? box.min.x + 14.0f : i == 1 ? box.Center().x - size.x * 0.5f : box.max.x - 14.0f - size.x;
        dl.AddText(font, 18.0f, {x, box.min.y + 16.0f + float(i) * 34.0f}, Hex(0x00BBF9), lines[i]);
    }
    dl.PopClipRect();

    // ---- Right: effects and scale -----------------------------------------------------------------------------------
    const float rx = 780.0f, rw = 460.0f;
    {
        const Rect panel(rx, 100.0f, rx + rw, 196.0f);
        dl.AddRectGradient(panel, Hex(0xF15BB5), Hex(0xFEE440), Gradient::Horizontal, 12.0f);
        TextStyle outline;
        outline.color = colors::White;
        outline.outlineWidth = 2.5f;
        outline.outlineColor = Hex(0x1A1A2E);
        dl.AddText(font, 48.0f, {panel.min.x + 22.0f, panel.min.y + 18.0f}, "Outlined text", outline);
    }
    {
        const Rect panel(rx, 208.0f, rx + rw, 304.0f);
        dl.AddRectFilled(panel, Hex(0xD8D8E4), 12.0f);
        TextStyle shadow;
        shadow.color = Hex(0x2A2A3A);
        shadow.shadowColor = Rgba(0, 0, 0, 120);
        shadow.shadowOffset = {3.0f, 4.0f};
        shadow.shadowSoftness = 2.5f;
        dl.AddText(font, 48.0f, {panel.min.x + 22.0f, panel.min.y + 18.0f}, "Drop shadow", shadow);
    }
    {
        const Rect panel(rx, 316.0f, rx + rw, 412.0f);
        dl.AddRectFilled(panel, Hex(0x101018), 12.0f);
        TextStyle glow;
        glow.color = colors::White;
        glow.shadowColor = Hex(0x00F5D4);
        glow.shadowOffset = {0.0f, 0.0f};
        glow.shadowSpread = 1.0f;
        glow.shadowSoftness = 2.5f;
        dl.AddText(font, 48.0f, {panel.min.x + 22.0f, panel.min.y + 18.0f}, "Neon glow", glow);
    }
    {
        const Rect panel(rx, 424.0f, rx + rw, 680.0f);
        dl.AddRectFilled(panel, kPanel, 12.0f);
        dl.PushClipRect(panel);
        dl.AddText(font, 12.0f, {panel.min.x + 16.0f, panel.min.y + 12.0f}, kCaption,
                   "Glyphs are generated at 48px; corners stay sharp far beyond that:");
        dl.AddText(font, 150.0f, {panel.min.x + 18.0f, panel.min.y + 26.0f}, Hex(0xFEE440), "Ag&");
        const float pulse = 18.0f + 14.0f * (0.5f + 0.5f * std::sin(time * 2.0f));
        dl.AddText(font, pulse, {panel.max.x - 130.0f, panel.max.y - 60.0f}, Hex(0x00BBF9), "scale");
        dl.PopClipRect();
    }
}

// =====================================================================================================================
// World-space 3D debug drawing
// =====================================================================================================================
namespace {

// Left-handed perspective (D3D clip: NDC z in [0,1], w = view-space z), row-major.
Mat4 PerspectiveLH(float fovY, float aspect, float nearZ, float farZ) {
    const float h = 1.0f / std::tan(fovY * 0.5f);
    const float w = h / aspect;
    Mat4 m;
    std::memset(m.m, 0, sizeof(m.m));
    m.m[0][0] = w;
    m.m[1][1] = h;
    m.m[2][2] = farZ / (farZ - nearZ);
    m.m[2][3] = -nearZ * farZ / (farZ - nearZ);
    m.m[3][2] = 1.0f;
    return m;
}

Mat4 LookAtLH(Vec3 eye, Vec3 target, Vec3 up) {
    const Vec3 f = Normalize(target - eye);
    const Vec3 r = Normalize(Cross(up, f));
    const Vec3 u = Cross(f, r);
    Mat4 m;
    m.m[0][0] = r.x; m.m[0][1] = r.y; m.m[0][2] = r.z; m.m[0][3] = -Dot(r, eye);
    m.m[1][0] = u.x; m.m[1][1] = u.y; m.m[1][2] = u.z; m.m[1][3] = -Dot(u, eye);
    m.m[2][0] = f.x; m.m[2][1] = f.y; m.m[2][2] = f.z; m.m[2][3] = -Dot(f, eye);
    m.m[3][0] = 0.0f; m.m[3][1] = 0.0f; m.m[3][2] = 0.0f; m.m[3][3] = 1.0f;
    return m;
}

Mat4 TranslateRotateY(Vec3 pos, float angle) {
    const float c = std::cos(angle), s = std::sin(angle);
    Mat4 m;
    m.m[0][0] = c; m.m[0][2] = s; m.m[0][3] = pos.x;
    m.m[1][1] = 1.0f; m.m[1][3] = pos.y;
    m.m[2][0] = -s; m.m[2][2] = c; m.m[2][3] = pos.z;
    return m;
}

} // namespace

void BuildDebug3DScene(DrawList& dl, float time, const SceneAssets& assets, Vec2 size) {
    dl.AddRectFilled(Rect({0.0f, 0.0f}, size), Hex(0x0B0D13));
    const float aspect = size.y > 0.0f ? size.x / size.y : 1.0f;
    const Vec3 eye{std::cos(time * 0.35f) * 9.0f, 5.0f, std::sin(time * 0.35f) * 9.0f};
    const Mat4 viewProj = PerspectiveLH(60.0f * kPi / 180.0f, aspect, 0.1f, 100.0f) * LookAtLH(eye, {0, 0, 0}, {0, 1, 0});

    DebugDraw3D dd;
    dd.Begin(viewProj, Rect({0.0f, 0.0f}, size), &dl);

    // Ground grid.
    for (int i = -6; i <= 6; ++i) {
        const Color c = i == 0 ? Hex(0x39507E) : Hex(0x1C2436);
        dd.Line({float(i), 0.0f, -6.0f}, {float(i), 0.0f, 6.0f}, c, 1.0f);
        dd.Line({-6.0f, 0.0f, float(i)}, {6.0f, 0.0f, float(i)}, c, 1.0f);
    }
    dd.AxisGizmo(Mat4(), 1.5f, 2.0f);

    // Labeled "entities": a box, a head sphere, and a billboard name.
    struct Entity {
        Vec3 pos;
        float height;
        Color color;
        const char* name;
    };
    const Entity entities[] = {{{2.5f, 0.0f, 1.0f}, 1.2f, Hex(0xFF5A5A), "Enemy"},
                               {{-2.5f, 0.0f, -1.5f}, 1.0f, Hex(0x5AC8FF), "Ally"},
                               {{0.0f, 0.0f, 3.0f}, 1.6f, Hex(0xFFD15A), "Boss"}};
    for (const Entity& e : entities) {
        const float hw = 0.45f;
        dd.Box({e.pos.x - hw, 0.0f, e.pos.z - hw}, {e.pos.x + hw, e.height, e.pos.z + hw}, e.color, 2.0f);
        dd.Sphere({e.pos.x, e.height + 0.35f, e.pos.z}, 0.3f, e.color, 16, 1.5f);
        if (assets.font) {
            dd.Text({e.pos.x, e.height + 0.95f, e.pos.z}, *assets.font, 15.0f, colors::White, e.name);
        }
    }

    // A spinning oriented box and an arrow pointing at the boss.
    dd.BoxOriented(TranslateRotateY({-3.5f, 0.8f, 3.0f}, time * 1.2f), {0.6f, 0.6f, 0.6f}, Hex(0x9B8CFF), 2.0f);
    dd.Arrow({4.0f, 2.5f, -3.0f}, {0.0f, 1.6f, 3.0f}, Hex(0x8AE05A), 2.0f);
    dd.Sphere({3.5f, 1.5f, -3.0f}, 0.5f, Hex(0x00F5D4), 20, 1.5f);

    if (assets.font) {
        TextStyle title;
        title.outlineWidth = 1.5f;
        title.outlineColor = Rgba(0, 0, 0, 220);
        dl.AddText(*assets.font, 20.0f, {24.0f, 20.0f}, "World-space 3D debug draw", title);
        dl.AddText(*assets.font, 14.0f, {24.0f, 48.0f}, Rgba(255, 255, 255, 150),
                   "lines, boxes, spheres, arrows, gizmos and billboard text, projected with a view-projection matrix");
    }
}

// =====================================================================================================================
// Stress scenes
// =====================================================================================================================
void StressScene::Generate(uint32_t count, Vec2 area) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    m_items.resize(count);
    for (Item& it : m_items) {
        const float s = 6.0f + u01(rng) * 22.0f;
        it.rect = Rect::FromPosSize({u01(rng) * (area.x - s), u01(rng) * (area.y - s)}, {s, s});
        it.drift = Vec2(u01(rng) - 0.5f, u01(rng) - 0.5f) * 40.0f;
        it.color = Hsv(u01(rng), 0.65f, 0.95f, 0.7f + 0.3f * u01(rng));
        it.kind = uint8_t(rng() % 5);
    }
}

void StressScene::Build(DrawList& dl, float time) const {
    const float wave = std::sin(time);
    for (const Item& it : m_items) {
        const Rect r = it.rect.Translated(it.drift * wave);
        switch (it.kind) {
        case 0: dl.AddRectFilled(r, it.color, 4.0f); break;
        case 1: dl.AddCircleFilled(r.Center(), r.Width() * 0.5f, it.color); break;
        case 2: dl.AddRect(r, it.color, 0.0f, 2.0f); break;
        case 3: dl.AddLine(r.min, r.max, it.color, 2.0f); break;
        default: dl.AddRectFilled(r, it.color); break;
        }
    }
}

void TextStressScene::Generate(uint32_t count, Vec2 area) {
    static constexpr const char* kWords[] = {
        "health", "mana",  "quest",   "inventory", "level", "gold",   "sword",  "shield", "potion", "map",
        "score",  "ammo",  "reload",  "settings",  "audio", "video",  "pause",  "resume", "victory", "defeat",
        "armor",  "skill", "upgrade", "crafting",  "party", "online", "player", "enemy",  "boss",   "loot"};
    std::mt19937 rng(4321);
    std::uniform_real_distribution<float> u01(0.0f, 1.0f);
    m_labels.resize(count);
    m_text.clear();
    for (Label& label : m_labels) {
        label.textOffset = uint32_t(m_text.size());
        const int words = 2 + int(rng() % 3);
        for (int w = 0; w < words; ++w) {
            if (w) m_text.push_back(' ');
            const std::string_view word = kWords[rng() % std::size(kWords)];
            m_text.insert(m_text.end(), word.begin(), word.end());
        }
        label.textLength = uint32_t(m_text.size()) - label.textOffset;
        label.size = 11.0f + u01(rng) * 9.0f;
        label.pos = {u01(rng) * (area.x - 160.0f), u01(rng) * (area.y - label.size)};
        label.drift = Vec2(u01(rng) - 0.5f, u01(rng) - 0.5f) * 40.0f;
        label.color = Hsv(u01(rng), 0.45f, 1.0f);
    }
}

void TextStressScene::Build(DrawList& dl, const Font& font, float time) const {
    const float wave = std::sin(time);
    for (const Label& label : m_labels) {
        dl.AddText(font, label.size, label.pos + label.drift * wave, label.color,
                   std::string_view(m_text.data() + label.textOffset, label.textLength));
    }
}

} // namespace sandbox
