// Sandbox scenes. Everything here uses only the public drizzy API, so it doubles as usage examples.
#pragma once

#include "drizzy/draw_list.h"
#include "drizzy/font.h"

#include <vector>

namespace sandbox {

struct SceneAssets {
    const drizzy::Font* font = nullptr;
    drizzy::TextureId checker = drizzy::kNoTexture;
};

// Every shape type on a 1280x720 canvas, one feature per captioned cell.
void BuildShapesScene(drizzy::DrawList& dl, float time, const SceneAssets& assets);
// Sizes, effects, wrapping and measuring on a 1280x720 canvas.
void BuildTextScene(drizzy::DrawList& dl, float time, const SceneAssets& assets);
// World-space 3D debug drawing: an orbiting camera over a grid with labeled boxes, spheres, arrows and gizmos.
void BuildDebug3DScene(drizzy::DrawList& dl, float time, const SceneAssets& assets, drizzy::Vec2 displaySize);

std::vector<uint32_t> MakeCheckerPixels(uint32_t size);

// Many small independent shapes: the worst case for per-shape overhead.
class StressScene {
public:
    void Generate(uint32_t count, drizzy::Vec2 area);
    void Build(drizzy::DrawList& dl, float time) const;
    uint32_t Count() const { return uint32_t(m_items.size()); }

private:
    struct Item {
        drizzy::Rect rect;
        drizzy::Vec2 drift;
        drizzy::Color color;
        uint8_t kind;
    };
    std::vector<Item> m_items;
};

// Many short labels (2-4 words, 11-20 px): the worst case for text overhead.
class TextStressScene {
public:
    void Generate(uint32_t count, drizzy::Vec2 area);
    void Build(drizzy::DrawList& dl, const drizzy::Font& font, float time) const;
    uint32_t Count() const { return uint32_t(m_labels.size()); }

private:
    struct Label {
        drizzy::Vec2 pos;
        drizzy::Vec2 drift;
        float size;
        drizzy::Color color;
        uint32_t textOffset, textLength;
    };
    std::vector<Label> m_labels;
    std::vector<char> m_text;
};

} // namespace sandbox
