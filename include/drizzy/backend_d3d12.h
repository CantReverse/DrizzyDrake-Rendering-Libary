// drizzy_renderer - Direct3D 12 backend.
//
// Like the D3D11 backend it never creates a device, queue, swap chain or window. The host passes in its device at Init
// and a command list to record into at Render; the renderer only records commands, so it fits into an engine's frame
// without owning any submission or synchronization.
//
// It binds one root signature, one pipeline and its own shader-visible SRV heap, and issues one DrawIndexedInstanced
// per DrawCmd. Prims reach the GPU one of two ways:
//  - Draw lists given the renderer as their PrimAllocator (DrawList::SetPrimAllocator, Ui::SetPrimAllocator) write
//    straight into persistently mapped GPU buffers while they are recorded, so Render copies nothing.
//  - Any other list is copied into a per-frame upload buffer by Render.
// Texture uploads are recorded at the start of the next Render call, so creating a texture never blocks.
#pragma once

#include "drizzy/draw_list.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <vector>

namespace drizzy::d3d12 {

// Where per-frame prims live. Either way they cross PCIe once; this picks which side pays for it.
enum class PrimMemory {
    // VideoMemory when the device supports GPU upload heaps (Resizable BAR), SystemMemory otherwise.
    Auto,
    // Upload heap in system memory: cheapest CPU writes; the GPU reads across PCIe while drawing.
    SystemMemory,
    // GPU upload heap (needs Resizable BAR): the CPU writes across PCIe, the GPU reads local video memory (about twice
    // as fast for large UIs). Falls back to SystemMemory if unsupported.
    VideoMemory,
};

struct RendererDesc {
    ID3D12Device* device = nullptr;
    // How many frames the host keeps in flight. Render() rotates through this many sets of per-frame buffers.
    uint32_t framesInFlight = 2;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // render target format the pipeline is built for
    uint32_t sampleCount = 1;
    uint32_t initialPrimCapacity = 1u << 14;  // records; grows on demand
    uint32_t maxTextures = 1024;              // size of the renderer's shader-visible SRV heap
    PrimMemory primMemory = PrimMemory::Auto;
};

struct RenderParams {
    // How the render target expects colors: Srgb for UNORM targets, SrgbLinear for *_SRGB views, ScRgb / Hdr10 for HDR
    // swap chains. Colors look the same on all of them; on HDR, SDR white is shown at paperWhiteNits.
    ColorEncoding colorEncoding = ColorEncoding::Srgb;
    float paperWhiteNits = 200.0f;
    // The render target's format, when it differs from RendererDesc::rtvFormat (a pipeline for it is built on first
    // use, so a game switching to HDR needs no re-initialization). UNKNOWN: RendererDesc::rtvFormat.
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
    // When non-zero, bound as the only render target before drawing. Otherwise the host's bindings are kept; they must
    // not include a depth buffer, since the pipeline declares none.
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = {};
};

class Renderer final : public PrimAllocator {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(const RendererDesc& desc);
    // The GPU must be idle (or at least done with every frame this renderer recorded).
    void Shutdown();

    // Records drawing (and pending texture uploads) into `cmd`, a direct command list in the recording state. The
    // render target must be in D3D12_RESOURCE_STATE_RENDER_TARGET. Ends the frame being recorded into the renderer's
    // memory (see AllocatePrims).
    //
    // Frame contract: each call uses the next of `framesInFlight` buffer sets. Before calling Render, the GPU must have
    // finished executing the command list recorded by the call made `framesInFlight` calls earlier. Hosts that wait on
    // a per-frame fence before reusing their command allocator already satisfy this.
    void Render(ID3D12GraphicsCommandList* cmd, const DrawData& data, const RenderParams& params = {});
    void Render(ID3D12GraphicsCommandList* cmd, const DrawList& list, Vec2 displaySize, const RenderParams& params = {});
    // Ends the frame being recorded into the renderer's memory without drawing anything (a frame with nothing to draw).
    void SkipFrame() { EndRecording(); }

    // PrimAllocator. A frame's recording starts with the first allocation after a Render / SkipFrame; its prims stay
    // drawable until the next frame's recording starts. Recording memory rotates through framesInFlight + 1 sets, so
    // the frame contract above also covers it: no extra synchronization is needed.
    bool AllocatePrims(uint32_t minCount, uint32_t desiredCount, PrimChunk& chunk) override;

    // Creates an RGBA8 (straight alpha) texture. The upload is recorded by the next Render call.
    TextureId CreateTexture(uint32_t width, uint32_t height, const void* rgba, uint32_t rowPitch = 0);
    // Makes an existing texture drawable by creating a view of it in the renderer's heap. The renderer holds a
    // reference until DestroyTexture; the host keeps the resource in D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE.
    TextureId RegisterTexture(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc = nullptr);
    // Releases a texture once every frame that may still use it has finished.
    void DestroyTexture(TextureId texture);

    ID3D12DescriptorHeap* SrvHeap() const { return m_srvHeap.Get(); }
    const RenderStats& LastFrameStats() const { return m_stats; }
    // True when prims live in video memory (PrimMemory::VideoMemory or Auto on a device with GPU upload heaps).
    bool PrimsInVideoMemory() const { return m_primHeapType == D3D12_HEAP_TYPE_GPU_UPLOAD; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Frame {
        ComPtr<ID3D12Resource> primBuffer;  // lists in their own memory, copied at Render; persistently mapped
        GpuPrim* mapped = nullptr;
        uint32_t capacity = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;  // released when this frame's buffers are next reused
        std::vector<uint32_t> freedSlots;             // descriptor slots returned at the same point
    };
    // Recording memory for one recording frame: [0] every frame, more only while a frame outgrows it.
    struct RecordBlock {
        ComPtr<ID3D12Resource> buffer;  // persistently mapped
        GpuPrim* mapped = nullptr;
        uint32_t capacity = 0;
        uint32_t used = 0;
    };
    struct RecordSet {
        std::vector<RecordBlock> blocks;
        uint32_t activeBlocks = 0;
    };
    struct PendingUpload {
        ComPtr<ID3D12Resource> texture;
        ComPtr<ID3D12Resource> upload;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    };

    uint32_t AllocateSlot();
    bool SlotOf(TextureId texture, uint32_t& slot) const;
    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandle(uint32_t slot) const;
    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandle(uint32_t slot) const;
    void RecycleFrame(Frame& frame);
    void FlushUploads(ID3D12GraphicsCommandList* cmd, Frame& frame);
    ComPtr<ID3D12Resource> CreatePrimBuffer(uint32_t capacity, GpuPrim*& mapped);
    bool EnsurePrimCapacity(Frame& frame, uint32_t primCount);
    bool EnsureIndexCapacity(ID3D12GraphicsCommandList* cmd, Frame& frame, uint32_t primCount);
    bool BeginRecording();
    void EndRecording();
    const RecordBlock* FindBlock(uint32_t block) const;

    ID3D12PipelineState* PipelineFor(DXGI_FORMAT format);

    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12RootSignature> m_rootSignature;
    struct Pipeline {
        DXGI_FORMAT format;
        ComPtr<ID3D12PipelineState> state;
    };
    std::vector<Pipeline> m_pipelines;  // one per render target format; [0] is RendererDesc::rtvFormat
    uint32_t m_sampleCount = 1;
    ComPtr<ID3D12DescriptorHeap> m_srvHeap;
    uint32_t m_srvIncrement = 0;
    std::vector<ComPtr<ID3D12Resource>> m_slotResources;
    std::vector<uint32_t> m_freeSlots;
    std::vector<Frame> m_frames;
    uint32_t m_frameIndex = 0;
    std::vector<RecordSet> m_recordSets;  // framesInFlight + 1
    uint32_t m_recordSet = 0;             // the set of the current (or last) recording frame
    uint32_t m_recordSerial = 0;          // numbers recording frames; part of every block id
    uint32_t m_lastRecorded = 0;          // records the last recording frame took, to size the next one
    bool m_recording = false;
    std::vector<PendingUpload> m_pendingUploads;
    ComPtr<ID3D12Resource> m_indexBuffer;
    uint32_t m_indexCapacity = 0;  // in records
    uint32_t m_initialCapacity = 0;
    D3D12_HEAP_TYPE m_primHeapType = D3D12_HEAP_TYPE_UPLOAD;
    TextureId m_whiteTexture = kNoTexture;
    RenderStats m_stats;
};

} // namespace drizzy::d3d12
