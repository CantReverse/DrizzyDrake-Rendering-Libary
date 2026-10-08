// Shared sample code: a particles.js-style animated backdrop and a window that exercises every drizzy UI widget.
// Used by the sandbox (gallery scene) and the overlay example (mod menu).
#pragma once

#include "drizzy/ui.h"

#include <cstdint>
#include <vector>

namespace demo {

// The particles.js "network" look: drifting points joined by thin lines when they are near each other, and to the
// mouse cursor when it comes within reach. It fills any rectangle; the widget gallery uses it as its window
// background, drawn before the widgets so it sits under them.
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

// A window demonstrating every widget drizzy offers, grouped into tabs, over an animated ParticleBackground that
// follows the theme. Pass an image texture to show Image / ImageButton; pass `open` to make the window closable.
void ShowWidgetGallery(drizzy::Ui& ui, drizzy::TextureId image = drizzy::kNoTexture, bool* open = nullptr);

} // namespace demo
