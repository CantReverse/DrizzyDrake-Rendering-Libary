// Internal: what the D3D11 and D3D12 backends share - the shader's constant block and how a frame's parameters fill it.
#pragma once

#include "drizzy/draw_list.h"

namespace drizzy::backend {

constexpr uint32_t kGlobalLinear = 1u << 0;  // prim.hlsl GLOBAL_LINEAR
constexpr uint32_t kGlobalPq = 1u << 1;      // prim.hlsl GLOBAL_PQ

// Matches cbuffer Globals in prim.hlsl (D3D12 sets it as 8 root constants).
struct alignas(16) Constants {
    float ndcScale[2];
    uint32_t flags;
    float colorScale;
    float pixelScale;
    float pad[3];
};
static_assert(sizeof(Constants) == 32, "Constants must match prim.hlsl");

inline Constants MakeConstants(Vec2 targetSize, float scale, ColorEncoding encoding, float paperWhiteNits) {
    if (!(scale > 0.0f)) scale = 1.0f;
    const float nits = paperWhiteNits > 1.0f ? paperWhiteNits : 1.0f;
    Constants c = {};
    c.ndcScale[0] = 2.0f * scale / targetSize.x;
    c.ndcScale[1] = -2.0f * scale / targetSize.y;
    c.pixelScale = scale;
    switch (encoding) {
    case ColorEncoding::Srgb: c.flags = 0; c.colorScale = 1.0f; break;
    case ColorEncoding::SrgbLinear: c.flags = kGlobalLinear; c.colorScale = 1.0f; break;
    case ColorEncoding::ScRgb: c.flags = kGlobalLinear; c.colorScale = nits / 80.0f; break;
    case ColorEncoding::Hdr10: c.flags = kGlobalLinear | kGlobalPq; c.colorScale = nits / 10000.0f; break;
    }
    return c;
}

// A clip rect in list units as a scissor in target pixels.
struct Scissor {
    long left, top, right, bottom;
};
inline Scissor ToScissor(const Rect& clip, Vec2 targetSize, float scale) {
    auto px = [](float v, float limit) { return long(std::floor(Clamp(v, 0.0f, limit) + 0.5f)); };
    return {px(clip.min.x * scale, targetSize.x), px(clip.min.y * scale, targetSize.y),
            px(clip.max.x * scale, targetSize.x), px(clip.max.y * scale, targetSize.y)};
}

} // namespace drizzy::backend
