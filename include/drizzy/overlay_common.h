// drizzy_renderer - what the D3D11 and D3D12 overlay bridges share: per-frame cost figures and back buffer color
// detection (SDR, sRGB views, scRGB and HDR10 swap chains).
#pragma once

#include "drizzy/draw_list.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

namespace drizzy::overlay {

// What the overlay cost in the last frame it drew, measured inside the game.
struct OverlayStats {
    float renderCpuMs = 0.0f;  // CPU time inside Render (state save/restore, recording, submission; not waits)
    float gpuMs = 0.0f;        // GPU time of the overlay's own drawing, from timestamp queries (a few frames old)
    bool gpuValid = false;     // gpuMs holds a measurement
    uint32_t prims = 0;
    uint32_t drawCalls = 0;
    ColorEncoding encoding = ColorEncoding::Srgb;  // how the back buffer was written
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;      // the back buffer format
};

// Not set: the overlay works the color space out from the back buffer format and the display.
constexpr DXGI_COLOR_SPACE_TYPE kColorSpaceUnknown = DXGI_COLOR_SPACE_CUSTOM;

// How to write into a back buffer of `format`. `colorSpace` is what the game set with IDXGISwapChain3::SetColorSpace1
// (forward it from a hook); when unknown, a 10-bit back buffer on a display in HDR mode is taken as HDR10.
inline ColorEncoding DetectColorEncoding(IDXGISwapChain* swapChain, DXGI_FORMAT format,
                                         DXGI_COLOR_SPACE_TYPE colorSpace) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return ColorEncoding::SrgbLinear;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return ColorEncoding::ScRgb;  // FP16 swap chains are scRGB on Windows
    case DXGI_FORMAT_R10G10B10A2_UNORM: {
        if (colorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) return ColorEncoding::Hdr10;
        if (colorSpace != kColorSpaceUnknown || !swapChain) return ColorEncoding::Srgb;
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 desc = {};
        if (SUCCEEDED(swapChain->GetContainingOutput(&output)) && SUCCEEDED(output.As(&output6)) &&
            SUCCEEDED(output6->GetDesc1(&desc)) && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020) {
            return ColorEncoding::Hdr10;
        }
        return ColorEncoding::Srgb;
    }
    default:
        return ColorEncoding::Srgb;
    }
}

} // namespace drizzy::overlay
