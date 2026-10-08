// Shared sample code: animated backdrops for menu windows (or any rectangle). They draw into a DrawList clipped to
// their area, under the widgets, in a few hundred prims at most, and take their colors from the theme unless given
// their own.
#pragma once

#include "drizzy/draw_list.h"
#include "drizzy/font.h"

#include <cstdint>
#include <vector>

namespace demo {

// The particles.js "network" look: drifting points joined by thin lines when they are near each other, and to the
// mouse cursor when it comes within reach.
class ParticleBackground {
public:
    // Steps the animation inside `area` (screen pixels). Points live relative to the area, so they move with it, keep
    // a constant density, and stretch rather than reseed when it is resized - dragging a window edge stays smooth.
    void Update(float deltaTime, const drizzy::Rect& area);
    // Draws the area from the last Update into `dl`, which should already be clipped to it. Points within
    // cursorDistance of `mouse` link to it while it is inside the area; pass an off-screen position for no links.
    void Draw(drizzy::DrawList& dl, drizzy::Vec2 mouse) const;

    float density = 1.5f;            // points per 10,000 square pixels
    int maxCount = 250;
    float speed = 26.0f;             // drift speed, pixels/second
    float linkDistance = 130.0f;     // points closer than this are joined
    float cursorDistance = 170.0f;   // points closer than this to the mouse link to it
    drizzy::Color background = drizzy::colors::Transparent;  // fill under the points (transparent: none)
    drizzy::Color lineColor = drizzy::colors::White;
    drizzy::Color pointColor = drizzy::colors::White;
    drizzy::Color cursorColor = drizzy::Hex(0x66E0FF);

private:
    struct Node {
        drizzy::Vec2 pos;  // relative to the area's top-left corner
        drizzy::Vec2 vel;
        float age;         // seconds since it appeared; new points fade in
    };
    float Random01();

    std::vector<Node> m_nodes;
    drizzy::Rect m_area;
    uint32_t m_rng = 20251007u;  // fixed seed: screenshots are reproducible
};

enum class BackdropKind : uint8_t {
    None,
    Constellation,  // drifting points linked by lines, reaching for the mouse
    MatrixRain,     // falling columns of glyphs
    Starfield,      // stars streaking out from the center
    Synthwave,      // a sunset over a scrolling neon grid
    Waves,          // layered translucent waves
    Bokeh,          // large soft circles drifting upward
    Snow,           // falling, swaying flakes
    Gradient,       // slowly shifting color gradient
    Count
};
const char* BackdropName(BackdropKind kind);

struct BackdropColors {
    drizzy::Color primary = drizzy::colors::White;  // points, lines, glyphs
    drizzy::Color accent = drizzy::Hex(0x5B8CFF);   // highlights: links to the mouse, rain heads, the sun
    drizzy::Color fill = drizzy::colors::Transparent;  // under everything (transparent: none)
};

class Backdrop {
public:
    // Steps and draws one frame into `dl` (clipped to `area`). `font` is used by the matrix rain; `mouse` by the
    // constellation (off-screen: no links).
    void Draw(drizzy::DrawList& dl, const drizzy::Rect& area, float deltaTime, const drizzy::Font* font,
              const BackdropColors& colors, drizzy::Vec2 mouse);

    BackdropKind kind = BackdropKind::Constellation;
    float speed = 1.0f;    // animation speed multiplier
    float density = 1.0f;  // particle count multiplier
    float opacity = 1.0f;  // scales every color's alpha

private:
    struct Particle {
        drizzy::Vec2 pos;
        drizzy::Vec2 vel;
        float size;
        float phase;
        float z;
    };
    struct Drop {
        float head;   // row of the leading glyph
        float speed;  // rows per second
        int length;   // glyphs in the trail
        uint32_t seed;
    };
    float Random01();
    void Reset(BackdropKind kind, const drizzy::Rect& area);

    ParticleBackground m_constellation;
    std::vector<Particle> m_particles;
    std::vector<Drop> m_drops;
    BackdropKind m_built = BackdropKind::None;
    drizzy::Vec2 m_size;
    float m_density = 0.0f;
    float m_time = 0.0f;
    uint32_t m_rng = 1234567u;
};

} // namespace demo
