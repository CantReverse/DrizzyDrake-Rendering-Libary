// drizzy_renderer - Direct3D 12 overlay bridge.
//
// Like D3D11Overlay, but for D3D12, where drawing needs the command queue the game presents with. The overlay first
// asks the swap chain for it; if the runtime does not hand it out, capture it in your hooks (hook
// ID3D12CommandQueue::ExecuteCommandLists and feed D3D12QueueCapture) and pass it with SetCommandQueue. The overlay
// owns its own command allocators, list, RTV heap, descriptor heap and fence, so it never disturbs the game's command
// recording.
//
//   // in your Present hook, before the real Present:
//   ui.NewFrame();  BuildMenu(ui);
//   overlay.Render(swapChain, ui.Render());
//   // in your ResizeBuffers hook, before the real ResizeBuffers:
//   overlay.OnResizeBuffers();
//
// Like the D3D11 bridge it copes with resizes, fullscreen switches, buffer count and format changes (HDR on and off),
// a new device or queue (it re-creates itself and calls the device callback), and device removal.
#pragma once

#include "drizzy/backend_d3d12.h"
#include "drizzy/overlay_common.h"

#include <atomic>
#include <functional>
#include <vector>

namespace drizzy::overlay {

// Finds the command queue a D3D12 game presents with, for when the swap chain will not tell: hook
// ID3D12CommandQueue::ExecuteCommandLists and feed every call here; the present queue is the DIRECT-type queue that
// submits work, and when several exist the busiest one is it. Call OnPresent once per Present: queues that stopped
// submitting (destroyed ones) are dropped. Hand PresentQueue() to D3D12Overlay::SetCommandQueue.
//
//   // in your ExecuteCommandLists hook (on the queue it was called on):
//   g_queueCapture.OnExecuteCommandLists(thisQueue);
//   // in your Present hook:
//   g_queueCapture.OnPresent();
//   if (auto* q = g_queueCapture.PresentQueue()) overlay.SetCommandQueue(q);
//
// Thread-safe (ExecuteCommandLists may be called from several threads). It keeps a reference to each queue it tracks,
// so PresentQueue() never returns a destroyed queue; references are released once a queue stops submitting.
class D3D12QueueCapture {
public:
    ~D3D12QueueCapture() { Reset(); }
    void OnExecuteCommandLists(ID3D12CommandQueue* queue);
    void OnPresent();
    ID3D12CommandQueue* PresentQueue() const { return m_best.load(std::memory_order_acquire); }
    // Optional: only accept queues created on this device (when you already know the swap chain's device).
    void SetDevice(ID3D12Device* device) { m_device = device; }
    void Reset();

private:
    struct Entry {
        ID3D12CommandQueue* queue = nullptr;  // referenced
        uint32_t count = 0;                   // submissions in the current window
        uint32_t lastSeen = 0;                // window index of the last submission
    };
    static constexpr uint32_t kMaxQueues = 8;
    static constexpr uint32_t kWindowFrames = 60;
    void Lock() {
        while (m_lock.test_and_set(std::memory_order_acquire)) {
        }
    }
    void Unlock() { m_lock.clear(std::memory_order_release); }

    std::atomic<ID3D12CommandQueue*> m_best{nullptr};
    std::atomic_flag m_lock = ATOMIC_FLAG_INIT;
    Entry m_entries[kMaxQueues];
    uint32_t m_entryCount = 0;
    uint32_t m_presents = 0;
    uint32_t m_window = 0;
    ID3D12Device* m_device = nullptr;
};

class D3D12Overlay {
public:
    D3D12Overlay() = default;
    ~D3D12Overlay();
    D3D12Overlay(const D3D12Overlay&) = delete;
    D3D12Overlay& operator=(const D3D12Overlay&) = delete;

    // The queue the game presents with, when the swap chain does not provide it (see D3D12QueueCapture). Cheap to call
    // every frame; only a change has any effect.
    void SetCommandQueue(ID3D12CommandQueue* queue);

    // Records and submits drawing of `data` over the swap chain's current back buffer, on the game's queue, before the
    // game's own Present runs. Returns false (drawing nothing) until a queue is known and setup succeeds. Safe to call
    // every frame from a Present hook. When `data` holds nothing to draw (a closed menu), it returns right away: no
    // command list, no submission, no fence.
    bool Render(IDXGISwapChain* swapChain, const DrawData& data);

    // Sets up the device and renderer without drawing (Render does it lazily otherwise), so textures can be created up
    // front. Returns false on failure (e.g. no queue known yet).
    bool Initialize(IDXGISwapChain* swapChain);

    // Call before forwarding ResizeBuffers to the game: waits for the overlay's own submissions to finish (it holds no
    // back buffer between frames) and looks at the back buffer format again.
    void OnResizeBuffers();

    // HDR: the color space the game set with IDXGISwapChain3::SetColorSpace1 (forward it from a hook; without it, a
    // 10-bit back buffer on a display in HDR mode is taken as HDR10), and how bright SDR white is on HDR (nits).
    void SetColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace);
    void SetPaperWhiteNits(float nits) { m_paperWhiteNits = nits; }
    float PaperWhiteNits() const { return m_paperWhiteNits; }

    // Called after the overlay (re)creates its renderer - on first use, and again when the game moves to a new device
    // or queue. Create (or re-create) the textures you draw with there: textures of the old device are gone.
    void SetDeviceCallback(std::function<void(d3d12::Renderer&)> callback) { m_onDevice = std::move(callback); }

    void Shutdown();  // waits for the GPU, then releases everything. Called on destruction.

    bool Initialized() const { return m_device != nullptr; }
    const OverlayStats& LastFrameStats() const { return m_stats; }
    // The underlying renderer, for creating textures. It is also a PrimAllocator: ui.SetPrimAllocator(&overlay.Renderer())
    // makes the UI record straight into GPU memory.
    d3d12::Renderer& Renderer() { return m_renderer; }
    ID3D12Device* Device() const { return m_device.Get(); }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    // The overlay's own frames in flight: allocator, fence value and timestamp pair per slot. Independent of the swap
    // chain's buffer count, so the game can change that freely.
    static constexpr uint32_t kFrames = 3;
    struct FrameSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        uint64_t fenceValue = 0;
        bool timed = false;
    };

    bool EnsureSetup(IDXGISwapChain* swapChain);
    bool WaitForFence(uint64_t value, uint32_t timeoutMs);
    void WaitForGpu();

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    ComPtr<ID3D12Fence> m_fence;
    ComPtr<ID3D12QueryHeap> m_queryHeap;
    ComPtr<ID3D12Resource> m_queryReadback;
    double m_timestampToMs = 0.0;
    void* m_fenceEvent = nullptr;
    uint64_t m_fenceValue = 0;
    uint32_t m_frameCounter = 0;
    bool m_removed = false;            // the device was removed: wait for the game to bring up a new one
    void* m_swapChain = nullptr;       // identity only, for the color encoding
    DXGI_FORMAT m_formatSeen = DXGI_FORMAT_UNKNOWN;
    ColorEncoding m_encoding = ColorEncoding::Srgb;
    DXGI_COLOR_SPACE_TYPE m_colorSpace = kColorSpaceUnknown;
    float m_paperWhiteNits = 200.0f;
    std::function<void(d3d12::Renderer&)> m_onDevice;
    FrameSlot m_frames[kFrames];
    d3d12::Renderer m_renderer;
    OverlayStats m_stats;
};

} // namespace drizzy::overlay
