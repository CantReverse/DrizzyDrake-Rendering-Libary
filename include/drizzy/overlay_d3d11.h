// drizzy_renderer - Direct3D 11 overlay bridge.
//
// For rendering drizzy over a swap chain you do not own - the intended case is an in-game overlay injected into your
// own game for mod support, drawing from inside a hooked IDXGISwapChain::Present. You give it the swap chain; it pulls
// the device out, makes a render target view for the current back buffer, and draws your DrawData over the game's
// frame. It saves and restores the device context's state, so the game's own rendering is unaffected.
//
//   // once: build a Ui + FontAtlas as usual.
//   // inside your Present hook, before calling the real Present:
//   ui.NewFrame();  BuildMenu(ui);
//   overlay.Render(swapChain, ui.Render());
//   // inside your ResizeBuffers hook, before calling the real ResizeBuffers:
//   overlay.OnResizeBuffers();
//
// It copes with what games do to their swap chain: resizes and fullscreen switches (it holds no back buffer between
// frames), HDR (FP16 scRGB and HDR10 back buffers are detected and written correctly), a new device (it re-creates
// itself and calls the device callback so textures can be re-uploaded) and device removal (it stops drawing until the
// game has a new device).
#pragma once

#include "drizzy/backend_d3d11.h"
#include "drizzy/overlay_common.h"

#include <functional>

namespace drizzy::overlay {

class D3D11Overlay {
public:
    D3D11Overlay() = default;
    ~D3D11Overlay();
    D3D11Overlay(const D3D11Overlay&) = delete;
    D3D11Overlay& operator=(const D3D11Overlay&) = delete;

    // Draws `data` over the swap chain's current back buffer. Initializes from the swap chain's device the first time,
    // and whenever the device changes. Returns false if the device could not be obtained or setup failed; in that case
    // it draws nothing and leaves the game untouched. Safe to call every frame from a Present hook. When `data` holds
    // nothing to draw (a closed menu), it returns right away without touching the swap chain, the context or the GPU.
    bool Render(IDXGISwapChain* swapChain, const DrawData& data);

    // Sets up the device and renderer from the swap chain without drawing (Render does it lazily otherwise), so
    // textures can be created up front. Returns false on failure.
    bool Initialize(IDXGISwapChain* swapChain) { return EnsureDevice(swapChain); }

    // Call before forwarding ResizeBuffers to the game. The overlay holds no back buffer between frames, so this only
    // makes it look at the back buffer format again (a resize can switch the game to or from HDR).
    void OnResizeBuffers() { m_formatSeen = DXGI_FORMAT_UNKNOWN; }

    // HDR: the color space the game set with IDXGISwapChain3::SetColorSpace1 (forward it from a hook; without it, a
    // 10-bit back buffer on a display in HDR mode is taken as HDR10), and how bright SDR white is on HDR (nits).
    void SetColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);
    void SetPaperWhiteNits(float nits) { m_paperWhiteNits = nits; }
    float PaperWhiteNits() const { return m_paperWhiteNits; }

    // Called after the overlay (re)creates its renderer - on first use, and again when the game switches to a new
    // device. Create (or re-create) the textures you draw with there: textures of the old device are gone.
    void SetDeviceCallback(std::function<void(d3d11::Renderer&)> callback) { m_onDevice = std::move(callback); }

    // Releases everything. Called automatically on destruction. After a Shutdown the next Render re-initializes.
    void Shutdown();

    bool Initialized() const { return m_device != nullptr; }
    const OverlayStats& LastFrameStats() const { return m_stats; }
    // The underlying renderer, for creating textures to draw with (e.g. mod icons). It is also a PrimAllocator:
    // ui.SetPrimAllocator(&overlay.Renderer()) makes the UI record straight into GPU memory.
    d3d11::Renderer& Renderer() { return m_renderer; }
    ID3D11Device* Device() const { return m_device.Get(); }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Timing {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending = false;
    };
    static constexpr uint32_t kTimings = 4;

    bool EnsureDevice(IDXGISwapChain* swapChain);
    void ReadTimings();

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    d3d11::Renderer m_renderer;
    void* m_swapChain = nullptr;         // identity only, to notice a different swap chain
    void* m_removedDevice = nullptr;     // identity only: a removed device is not set up again...
    void* m_removedSwapChain = nullptr;  // ...until the game presents with another swap chain
    DXGI_FORMAT m_formatSeen = DXGI_FORMAT_UNKNOWN;
    ColorEncoding m_encoding = ColorEncoding::Srgb;
    DXGI_COLOR_SPACE_TYPE m_colorSpace = kColorSpaceUnknown;
    float m_paperWhiteNits = 200.0f;
    std::function<void(d3d11::Renderer&)> m_onDevice;
    Timing m_timings[kTimings];
    uint32_t m_timingIndex = 0;
    OverlayStats m_stats;
};

} // namespace drizzy::overlay
