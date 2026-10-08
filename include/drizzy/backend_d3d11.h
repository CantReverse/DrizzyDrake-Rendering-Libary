// drizzy_renderer - Direct3D 11 backend.
//
// The backend never creates a device, swap chain or window: the host (an engine plugin or an app) hands it a device
// and context and binds the render target to draw into. It issues one DrawIndexed per DrawCmd.
//
// Prims reach the GPU one of two ways:
//  - Draw lists given the renderer as their PrimAllocator (DrawList::SetPrimAllocator, Ui::SetPrimAllocator) write
//    straight into a mapped dynamic buffer while they are recorded, so Render copies nothing.
//  - Any other list is copied into an upload buffer by Render (one map per frame).
#pragma once

#include "drizzy/draw_list.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <vector>

namespace drizzy::d3d11 {

struct RendererDesc {
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    uint32_t initialPrimCapacity = 1u << 14;  // records; grows on demand
};

struct RenderParams {
    // How the bound render target expects colors: Srgb for UNORM targets, SrgbLinear for *_SRGB views, ScRgb / Hdr10
    // for HDR swap chains. Colors look the same on all of them; on HDR, SDR white is shown at paperWhiteNits.
    ColorEncoding colorEncoding = ColorEncoding::Srgb;
    float paperWhiteNits = 200.0f;
    // Save the context's pipeline state before drawing and restore it afterwards (~50 extra API calls). Only needed when
    // the host relies on its state surviving the call.
    bool preserveState = false;
};

class Renderer final : public PrimAllocator {
public:
    Renderer() = default;
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool Init(const RendererDesc& desc);
    void Shutdown();

    // Draws into the render target currently bound to the output-merger stage, with the viewport covering
    // data.displaySize. Ends the frame being recorded into the renderer's memory (see AllocatePrims).
    void Render(const DrawData& data, const RenderParams& params = {});
    void Render(const DrawList& list, Vec2 displaySize, const RenderParams& params = {});
    // Ends the frame being recorded into the renderer's memory without drawing anything (a frame with nothing to draw).
    void SkipFrame() { EndRecording(); }

    // PrimAllocator. A frame's recording starts with the first allocation after a Render / SkipFrame and maps the
    // renderer's dynamic buffer through the device context, so record on the context's thread. Its prims stay drawable
    // until the next frame's recording starts.
    bool AllocatePrims(uint32_t minCount, uint32_t desiredCount, PrimChunk& chunk) override;

    // Creates an immutable RGBA8 (straight alpha) texture. Release it with DestroyTexture.
    TextureId CreateTexture(uint32_t width, uint32_t height, const void* rgba, uint32_t rowPitch = 0);
    void DestroyTexture(TextureId texture);
    // Wraps an existing view. The caller keeps ownership and must keep it alive while it is in use.
    static TextureId FromSRV(ID3D11ShaderResourceView* srv) { return TextureId(reinterpret_cast<uintptr_t>(srv)); }

    const RenderStats& LastFrameStats() const { return m_stats; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct SavedState;
    // A dynamic structured buffer of prims and its view.
    struct PrimBuffer {
        ComPtr<ID3D11Buffer> buffer;
        ComPtr<ID3D11ShaderResourceView> srv;
        uint32_t capacity = 0;
        GpuPrim* mapped = nullptr;  // while recording
        uint32_t used = 0;          // records handed out this recording frame
    };

    bool CreatePrimBuffer(PrimBuffer& buffer, uint32_t capacity);
    bool EnsureIndexCapacity(uint32_t primCount);
    bool BeginRecording();
    void EndRecording();
    const PrimBuffer* FindBlock(uint32_t block) const;
    void SaveState(SavedState& s) const;
    void RestoreState(SavedState& s) const;

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<ID3D11VertexShader> m_vs;
    ComPtr<ID3D11PixelShader> m_ps;
    ComPtr<ID3D11Buffer> m_constants;
    ComPtr<ID3D11Buffer> m_indexBuffer;
    ComPtr<ID3D11BlendState> m_blend;
    ComPtr<ID3D11RasterizerState> m_rasterizer;
    ComPtr<ID3D11DepthStencilState> m_depthStencil;
    ComPtr<ID3D11SamplerState> m_sampler;
    ComPtr<ID3D11ShaderResourceView> m_whiteSrv;
    PrimBuffer m_copyBuffer;            // lists in their own memory, copied at Render
    std::vector<PrimBuffer> m_blocks;   // recording memory: [0] every frame, more only while a frame outgrows it
    uint32_t m_activeBlocks = 0;        // blocks handed out by the current (or last) recording frame
    uint32_t m_recordSerial = 0;        // numbers recording frames; part of every block id
    uint32_t m_lastRecorded = 0;        // records the last recording frame took, to size the next one
    bool m_recording = false;           // blocks are mapped
    uint32_t m_indexCapacity = 0;       // records
    uint32_t m_initialCapacity = 0;
    float m_lastConstants[8] = {};  // backend::Constants last uploaded
    bool m_constantsValid = false;
    RenderStats m_stats;
};

} // namespace drizzy::d3d11
