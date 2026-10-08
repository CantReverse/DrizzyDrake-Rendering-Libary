#include "demo_backdrops.h"

#include <algorithm>
#include <cmath>

using namespace drizzy;

namespace demo {

// =====================================================================================================================
// ParticleBackground
// =====================================================================================================================
float ParticleBackground::Random01() {
    m_rng ^= m_rng << 13;  // xorshift32
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return float(m_rng >> 8) * (1.0f / 16777216.0f);
}

void ParticleBackground::Update(float deltaTime, const Rect& area) {
    const Vec2 size = area.Size();
    const Vec2 oldSize = m_area.Size();
    m_area = area;
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    if (oldSize.x > 0.0f && oldSize.y > 0.0f && oldSize != size) {
        const Vec2 scale(size.x / oldSize.x, size.y / oldSize.y);
        for (Node& node : m_nodes) node.pos = {node.pos.x * scale.x, node.pos.y * scale.y};
    }
    // Constant density: a growing area gains points (they fade in), a shrinking one drops them. The first seeding
    // shows at once, so a single-frame screenshot is fully populated.
    const int target = std::clamp(int(size.x * size.y * density * 1e-4f), 0, std::max(maxCount, 0));
    const float startAge = m_nodes.empty() ? 1.0f : 0.0f;
    if (int(m_nodes.size()) > target) m_nodes.resize(size_t(target));
    while (int(m_nodes.size()) < target) {
        const float angle = Random01() * 2.0f * kPi;
        const Vec2 pos(Random01() * size.x, Random01() * size.y);
        m_nodes.push_back({pos, {std::cos(angle) * speed, std::sin(angle) * speed}, startAge});
    }

    deltaTime = Clamp(deltaTime, 0.0f, 0.05f);  // stay stable after a long stall
    for (Node& node : m_nodes) {
        node.age += deltaTime;
        node.pos += node.vel * deltaTime;
        // Bounce off the edges, so the field stays evenly spread.
        if (node.pos.x < 0.0f) { node.pos.x = 0.0f; node.vel.x = std::fabs(node.vel.x); }
        else if (node.pos.x > size.x) { node.pos.x = size.x; node.vel.x = -std::fabs(node.vel.x); }
        if (node.pos.y < 0.0f) { node.pos.y = 0.0f; node.vel.y = std::fabs(node.vel.y); }
        else if (node.pos.y > size.y) { node.pos.y = size.y; node.vel.y = -std::fabs(node.vel.y); }
    }
}

void ParticleBackground::Draw(DrawList& dl, Vec2 mouse) const {
    if (ColorAlpha(background)) dl.AddRectFilled(m_area, background);
    const Vec2 origin = m_area.min;
    const float linkSq = linkDistance * linkDistance;
    auto fadeIn = [](const Node& node) { return std::min(node.age * 2.0f, 1.0f); };

    // Point-to-point links: alpha fades with distance. O(n^2), fine for a few hundred points.
    for (size_t i = 0; i < m_nodes.size(); ++i) {
        const Node& a = m_nodes[i];
        for (size_t j = i + 1; j < m_nodes.size(); ++j) {
            const Node& b = m_nodes[j];
            const float d2 = LengthSq(b.pos - a.pos);
            if (d2 >= linkSq) continue;
            const float t = (1.0f - std::sqrt(d2) / linkDistance) * std::min(fadeIn(a), fadeIn(b));
            dl.AddLine(origin + a.pos, origin + b.pos, ScaleAlpha(lineColor, t * 0.4f), 1.0f);
        }
    }
    // Point-to-cursor links: brighter, so the field visibly reaches for the mouse as it moves.
    if (m_area.Contains(mouse)) {
        const float cursorSq = cursorDistance * cursorDistance;
        for (const Node& node : m_nodes) {
            const Vec2 p = origin + node.pos;
            const float d2 = LengthSq(p - mouse);
            if (d2 >= cursorSq) continue;
            const float t = std::min((1.0f - std::sqrt(d2) / cursorDistance) * 1.6f, 1.0f) * fadeIn(node);
            dl.AddLine(p, mouse, ScaleAlpha(cursorColor, t), 1.25f);
        }
    }
    // Points on top of the lines.
    for (const Node& node : m_nodes) dl.AddCircleFilled(origin + node.pos, 1.6f, ScaleAlpha(pointColor, fadeIn(node)));
}

// =====================================================================================================================
// Backdrop
// =====================================================================================================================
namespace {

const char* const kBackdropNames[] = {"None",  "Constellation", "Matrix rain", "Starfield", "Synthwave",
                                      "Waves", "Bokeh",         "Snow",        "Gradient"};
static_assert(sizeof(kBackdropNames) / sizeof(kBackdropNames[0]) == size_t(BackdropKind::Count),
              "kBackdropNames must match BackdropKind");

constexpr float kRainGlyphSize = 14.0f;

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

Color Mix(Color a, Color b, float t) {
    auto channel = [&](int shift) {
        const float x = float((a >> shift) & 0xFFu), y = float((b >> shift) & 0xFFu);
        return uint32_t(x + (y - x) * t + 0.5f) << shift;
    };
    return channel(0) | channel(8) | channel(16) | channel(24);
}

} // namespace

const char* BackdropName(BackdropKind kind) {
    return size_t(kind) < size_t(BackdropKind::Count) ? kBackdropNames[size_t(kind)] : "";
}

float Backdrop::Random01() {
    m_rng ^= m_rng << 13;
    m_rng ^= m_rng >> 17;
    m_rng ^= m_rng << 5;
    return float(m_rng >> 8) * (1.0f / 16777216.0f);
}

void Backdrop::Reset(BackdropKind newKind, const Rect& area) {
    m_built = newKind;
    m_density = density;
    m_rng = 1234567u;  // the same scene every time it is picked, so screenshots are reproducible
    m_particles.clear();
    m_drops.clear();
    const float d = std::max(density, 0.05f);
    auto count = [&](float n) { return std::max(1, int(n * d)); };
    switch (newKind) {
    case BackdropKind::Starfield:
        for (int i = 0; i < count(160.0f); ++i) {
            m_particles.push_back({{Random01() * 2.0f - 1.0f, Random01() * 2.0f - 1.0f}, {}, 0.0f, Random01(),
                                   0.03f + Random01() * 0.97f});
        }
        break;
    case BackdropKind::Bokeh:
        for (int i = 0; i < count(26.0f); ++i) {
            m_particles.push_back({{Random01(), Random01()}, {(Random01() - 0.5f) * 10.0f, -(8.0f + Random01() * 16.0f)},
                                   12.0f + Random01() * 48.0f, Random01() * 6.28f, 0.0f});
        }
        break;
    case BackdropKind::Snow:
        for (int i = 0; i < count(170.0f); ++i) {
            const float size = 0.8f + Random01() * 2.4f;
            m_particles.push_back({{Random01(), Random01()}, {0.0f, 14.0f + size * 14.0f}, size, Random01() * 6.28f, 0.0f});
        }
        break;
    default:
        break;
    }
    (void)area;
}

void Backdrop::Draw(DrawList& dl, const Rect& area, float deltaTime, const Font* font, const BackdropColors& colorsIn,
                    Vec2 mouse) {
    const Vec2 size = area.Size();
    if (kind == BackdropKind::None || size.x <= 1.0f || size.y <= 1.0f) return;
    if (kind != m_built || density != m_density) Reset(kind, area);
    m_size = size;
    deltaTime = Clamp(deltaTime, 0.0f, 0.05f) * std::max(speed, 0.0f);
    m_time += deltaTime;
    const float t = m_time;
    const Color primary = ScaleAlpha(colorsIn.primary, opacity);
    const Color accent = ScaleAlpha(colorsIn.accent, opacity);
    if (ColorAlpha(colorsIn.fill)) dl.AddRectFilled(area, ScaleAlpha(colorsIn.fill, opacity));
    const Vec2 o = area.min;

    switch (kind) {
    case BackdropKind::Constellation: {
        m_constellation.density = 1.5f * density;
        m_constellation.speed = 26.0f;
        m_constellation.lineColor = m_constellation.pointColor = primary;
        m_constellation.cursorColor = accent;
        m_constellation.Update(deltaTime, area);
        m_constellation.Draw(dl, mouse);
        break;
    }
    case BackdropKind::MatrixRain: {
        if (!font) break;
        const float colW = kRainGlyphSize * 0.9f, rowH = kRainGlyphSize * 1.15f;
        const int columns = int(size.x / colW) + 1;
        const float rows = size.y / rowH;
        while (int(m_drops.size()) < columns) {
            m_drops.push_back({Random01() * rows * 1.2f, 5.0f + Random01() * 11.0f, 6 + int(Random01() * 18.0f),
                               Hash(uint32_t(m_drops.size()) * 977u + 13u)});
        }
        static const char kGlyphs[] = "0123456789ABCDEFXZ$#@%&*+=<>?";
        const int glyphCount = int(sizeof(kGlyphs)) - 1;
        const Color head = Mix(accent, WithAlpha(colors::White, ColorAlpha(accent)), 0.55f);
        for (int c = 0; c < columns; ++c) {
            Drop& drop = m_drops[size_t(c)];
            drop.head += drop.speed * deltaTime;
            if ((drop.head - float(drop.length)) > rows) {
                drop.head = -Random01() * 8.0f;
                drop.speed = 5.0f + Random01() * 11.0f;
                drop.length = 6 + int(Random01() * 18.0f);
            }
            const int headRow = int(std::floor(drop.head));
            for (int k = 0; k < drop.length; ++k) {
                const int row = headRow - k;
                if (row < 0 || float(row) > rows) continue;
                // Each cell flips to a new glyph a few times a second.
                const uint32_t h = Hash(uint32_t(c) * 7919u + uint32_t(row) * 104729u + uint32_t(t * 3.0f + float(drop.seed % 7u)));
                const char glyph = kGlyphs[h % uint32_t(glyphCount)];
                const float fade = 1.0f - float(k) / float(drop.length);
                const Color color = k == 0 ? head : ScaleAlpha(Mix(accent, primary, 0.25f), fade * fade * 0.8f);
                dl.AddText(*font, kRainGlyphSize, {o.x + float(c) * colW, o.y + float(row) * rowH}, color,
                           std::string_view(&glyph, 1));
            }
        }
        break;
    }
    case BackdropKind::Starfield: {
        const Vec2 center = area.Center();
        const float scale = std::max(size.x, size.y) * 0.5f;
        for (Particle& p : m_particles) {
            p.z -= deltaTime * 0.3f;
            Vec2 pos = center + p.pos * (scale * 0.25f / p.z);
            if (p.z <= 0.02f || !area.Expanded(4.0f).Contains(pos)) {
                p.pos = {Random01() * 2.0f - 1.0f, Random01() * 2.0f - 1.0f};
                p.z = 1.0f;
                continue;
            }
            const Vec2 tail = center + p.pos * (scale * 0.25f / std::min(p.z + 0.035f, 1.0f));
            const float near = 1.0f - p.z;
            const Color color = ScaleAlpha(p.phase > 0.85f ? accent : primary, Clamp(near * 0.9f, 0.0f, 0.75f));
            dl.AddLine(tail, pos, color, 0.5f + near * 1.1f, LineCap::Round);
        }
        break;
    }
    case BackdropKind::Synthwave: {
        const float horizon = std::floor(o.y + size.y * 0.62f);
        const float cx = area.Center().x;
        // Sky glow and the striped sun on the horizon.
        dl.AddRectGradient(Rect(o.x, o.y, area.max.x, horizon), WithAlpha(accent, 0), ScaleAlpha(accent, 0.3f),
                           Gradient::Vertical);
        const float r = std::min(size.x, size.y) * 0.17f;
        const Rect sun = Rect::FromCenter({cx, horizon - r * 0.35f}, {r, r});
        dl.AddShadow(sun, ScaleAlpha(accent, 0.45f), r * 0.6f, r);
        RectStyle sunStyle;
        sunStyle.fill = Mix(accent, WithAlpha(Hex(0xFFE27A), ColorAlpha(accent)), 0.75f);
        sunStyle.fillEnd = accent;
        sunStyle.gradient = Gradient::Vertical;
        sunStyle.radii = r;
        dl.AddRectEx(sun, sunStyle);
        for (int i = 0; i < 5; ++i) {
            const float y = sun.min.y + r * (1.0f + 0.22f * float(i)) + std::fmod(t * 6.0f, r * 0.22f);
            const float h = 1.5f + float(i) * 1.2f;
            if (y + h < horizon) dl.AddRectFilled(Rect(sun.min.x - 1.0f, y, sun.max.x + 1.0f, y + h), ScaleAlpha(colors::Black, 0.55f * opacity));
        }
        // The ground and its grid: lines fanning out from the vanishing point, and rows scrolling toward the viewer.
        dl.AddRectFilled(Rect(o.x, horizon, area.max.x, area.max.y), ScaleAlpha(colors::Black, 0.35f * opacity));
        const float depth = area.max.y - horizon;
        const Color line = Mix(accent, primary, 0.2f);
        const int fan = 12;
        const float spread = size.x / 6.0f;
        for (int i = -fan; i <= fan; ++i) {
            const Vec2 a(cx + float(i) * spread * 0.05f, horizon);
            const Vec2 b(cx + float(i) * spread, area.max.y);
            dl.AddLine(a, b, ScaleAlpha(line, 0.18f), 3.0f);
            dl.AddLine(a, b, ScaleAlpha(line, 0.75f), 1.0f);
        }
        const float phase = std::fmod(t * 0.9f, 1.0f);
        for (int k = 0; k < 14; ++k) {
            const float z = float(k) + 1.0f - phase;
            const float y = horizon + depth * (1.0f / (z * 0.45f + 1.0f)) * 1.0f;
            const float fade = Clamp((y - horizon) / depth * 1.6f, 0.0f, 1.0f);
            dl.AddLine({o.x, y}, {area.max.x, y}, ScaleAlpha(line, 0.18f * fade), 3.0f);
            dl.AddLine({o.x, y}, {area.max.x, y}, ScaleAlpha(line, 0.8f * fade), 1.0f);
        }
        dl.AddLine({o.x, horizon}, {area.max.x, horizon}, ScaleAlpha(Mix(accent, colors::White, 0.4f), 0.9f), 1.5f);
        break;
    }
    case BackdropKind::Waves: {
        Vec2 points[160];
        const float step = std::max(10.0f, size.x / 120.0f);
        const int n = std::min(int(size.x / step) + 2, 156);
        for (int layer = 0; layer < 4; ++layer) {
            const float l = float(layer);
            const float base = o.y + size.y * (0.52f + 0.11f * l);
            const float amp = size.y * 0.045f * (1.0f + 0.35f * l);
            int count = 0;
            points[count++] = {area.max.x, area.max.y};
            points[count++] = {o.x, area.max.y};
            for (int i = 0; i < n; ++i) {
                const float x = std::min(o.x + float(i) * step, area.max.x);
                const float y = base + amp * std::sin((x - o.x) * 0.010f * (1.0f + 0.3f * l) + t * (0.6f + 0.25f * l) + l * 1.7f) +
                                amp * 0.4f * std::sin((x - o.x) * 0.027f + t * 1.3f + l);
                points[count++] = {x, y};
            }
            const Color color = Mix(primary, accent, 0.35f + 0.2f * l);
            dl.AddConcavePolyFilled(points, uint32_t(count), ScaleAlpha(color, 0.06f + 0.025f * l));
            dl.AddPolyline(points + 2, uint32_t(count - 2), ScaleAlpha(color, 0.3f), 1.25f);
        }
        break;
    }
    case BackdropKind::Bokeh: {
        for (size_t i = 0; i < m_particles.size(); ++i) {
            Particle& p = m_particles[i];
            p.pos += Vec2(p.vel.x / size.x, p.vel.y / size.y) * deltaTime;
            const float margin = p.size / size.y;
            if (p.pos.y < -margin) p.pos.y = 1.0f + margin;
            if (p.pos.x < -0.1f) p.pos.x = 1.1f;
            else if (p.pos.x > 1.1f) p.pos.x = -0.1f;
            const Vec2 c(o.x + p.pos.x * size.x, o.y + p.pos.y * size.y);
            const float pulse = 0.5f + 0.5f * std::sin(t * 0.8f + p.phase);
            const Color color = ScaleAlpha(i % 3 == 0 ? accent : primary, 0.07f + 0.12f * pulse);
            dl.AddShadow(Rect::FromCenter(c, {p.size, p.size}), color, p.size * 0.35f, p.size);
            if (i % 4 == 1) dl.AddCircle(c, p.size, ScaleAlpha(color, 0.8f), 1.0f);
        }
        break;
    }
    case BackdropKind::Snow: {
        for (Particle& p : m_particles) {
            p.pos.y += p.vel.y * deltaTime / size.y;
            p.pos.x += std::sin(t * 1.3f + p.phase) * 10.0f * deltaTime / size.x;
            if (p.pos.y > 1.02f) {
                p.pos.y = -0.02f;
                p.pos.x = Random01();
            }
            dl.AddCircleFilled({o.x + p.pos.x * size.x, o.y + p.pos.y * size.y}, p.size,
                               ScaleAlpha(p.size > 2.6f ? accent : primary, 0.25f + p.size * 0.18f));
        }
        break;
    }
    case BackdropKind::Gradient: {
        // Corner colors that drift between the two colors, under three large soft blobs on slow Lissajous paths.
        auto corner = [&](int i) {
            const float k = 0.5f + 0.5f * std::sin(t * 0.5f + float(i) * 1.9f);
            return ScaleAlpha(Mix(accent, primary, k * 0.6f), 0.10f + 0.12f * k);
        };
        dl.AddRectFilledMultiColor(area, corner(0), corner(1), corner(2), corner(3));
        const float r = std::min(size.x, size.y) * 0.38f;
        for (int i = 0; i < 3; ++i) {
            const float fi = float(i);
            const Vec2 c(o.x + size.x * (0.5f + 0.35f * std::sin(t * (0.21f + 0.07f * fi) + fi * 2.1f)),
                         o.y + size.y * (0.5f + 0.35f * std::cos(t * (0.17f + 0.05f * fi) + fi * 1.3f)));
            dl.AddShadow(Rect::FromCenter(c, {r, r}), ScaleAlpha(i == 1 ? primary : accent, 0.16f), r * 0.9f, r);
        }
        break;
    }
    default:
        break;
    }
}

} // namespace demo
