#ifndef NOMINMAX
#define NOMINMAX  // keep <windows.h> (pulled in by d3d11.h) from defining min / max macros
#endif
#include "drizzy/backend_d3d11.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "../backend_common.h"
#include "prim_ps_d3d11.h"  // g_PrimPS, compiled from src/shaders/prim.hlsl at build time
#include "prim_vs_d3d11.h"  // g_PrimVS

namespace drizzy::d3d11 {
namespace {

// 16M records = 512 MiB of prim data; also keeps recordIndex * 4 + 3 well inside 32-bit indices.
constexpr uint32_t kMaxPrims = 1u << 24;
// Recording memory: a frame normally fits block 0; a frame that outgrows it opens up to this many in total, and the
// next frame's block 0 is sized for the whole lot.
constexpr uint32_t kMaxBlocks = 8;
constexpr uint32_t kSerialMask = 0x0FFFFFFFu;

// Block ids: recording frame serial << 4 | block index + 1. Never 0 (a list's own memory) and, with at most
// kMaxBlocks blocks, never 0xFFFFFFFF.
constexpr uint32_t BlockId(uint32_t serial, uint32_t index) { return (serial << 4) | (index + 1); }

using backend::Constants;

D3D11_RECT ToScissor(const Rect& clip, Vec2 size, float scale) {
    const backend::Scissor s = backend::ToScissor(clip, size, scale);
    return {LONG(s.left), LONG(s.top), LONG(s.right), LONG(s.bottom)};
}

uint32_t GrowCapacity(uint32_t current, uint32_t needed) {
    uint32_t capacity = current ? current : 256u;
    while (capacity < needed && capacity < kMaxPrims) capacity *= 2;
    return std::min(capacity, kMaxPrims);
}

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

} // namespace

struct Renderer::SavedState {
    UINT scissorCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    D3D11_RECT scissors[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    ID3D11RasterizerState* rasterizer = nullptr;
    ID3D11BlendState* blend = nullptr;
    FLOAT blendFactor[4] = {};
    UINT sampleMask = 0;
    ID3D11DepthStencilState* depthStencil = nullptr;
    UINT stencilRef = 0;
    ID3D11ShaderResourceView* vsSrv = nullptr;
    ID3D11ShaderResourceView* psSrv = nullptr;
    ID3D11SamplerState* psSampler = nullptr;
    ID3D11Buffer* vsConstants = nullptr;
    ID3D11Buffer* psConstants = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11GeometryShader* gs = nullptr;
    ID3D11HullShader* hs = nullptr;
    ID3D11DomainShader* ds = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11Buffer* indexBuffer = nullptr;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    ID3D11InputLayout* inputLayout = nullptr;
};

Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(const RendererDesc& desc) {
    Shutdown();
    if (!desc.device || !desc.context) return false;
    m_device = desc.device;
    m_context = desc.context;
    m_initialCapacity = std::clamp(desc.initialPrimCapacity, 256u, kMaxPrims);

    bool ok = SUCCEEDED(m_device->CreateVertexShader(g_PrimVS, sizeof(g_PrimVS), nullptr, &m_vs)) &&
              SUCCEEDED(m_device->CreatePixelShader(g_PrimPS, sizeof(g_PrimPS), nullptr, &m_ps));

    D3D11_BUFFER_DESC cb = {};
    cb.ByteWidth = sizeof(Constants);
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ok = ok && SUCCEEDED(m_device->CreateBuffer(&cb, nullptr, &m_constants));

    // Premultiplied alpha blending.
    D3D11_BLEND_DESC bd = {};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ok = ok && SUCCEEDED(m_device->CreateBlendState(&bd, &m_blend));

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.ScissorEnable = TRUE;
    rd.DepthClipEnable = TRUE;
    ok = ok && SUCCEEDED(m_device->CreateRasterizerState(&rd, &m_rasterizer));

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    ok = ok && SUCCEEDED(m_device->CreateDepthStencilState(&dsd, &m_depthStencil));

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ok = ok && SUCCEEDED(m_device->CreateSamplerState(&sd, &m_sampler));

    // Bound for draw commands without a texture so the shader never reads an unbound slot.
    const uint32_t white = 0xFFFFFFFFu;
    if (ok) {
        const TextureId whiteTex = CreateTexture(1, 1, &white);
        m_whiteSrv.Attach(reinterpret_cast<ID3D11ShaderResourceView*>(whiteTex));
        ok = whiteTex != kNoTexture;
    }

    ok = ok && CreatePrimBuffer(m_copyBuffer, m_initialCapacity) && EnsureIndexCapacity(m_initialCapacity);
    if (!ok) Shutdown();
    return ok;
}

void Renderer::Shutdown() {
    EndRecording();
    m_blocks.clear();
    m_activeBlocks = 0;
    m_lastRecorded = 0;
    m_copyBuffer = {};
    m_whiteSrv.Reset();
    m_sampler.Reset();
    m_depthStencil.Reset();
    m_rasterizer.Reset();
    m_blend.Reset();
    m_indexBuffer.Reset();
    m_indexCapacity = 0;
    m_constants.Reset();
    m_ps.Reset();
    m_vs.Reset();
    m_context.Reset();
    m_device.Reset();
    m_constantsValid = false;
}

bool Renderer::CreatePrimBuffer(PrimBuffer& out, uint32_t capacity) {
    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = capacity * UINT(sizeof(GpuPrim));
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    bd.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    bd.StructureByteStride = sizeof(GpuPrim);
    PrimBuffer buffer;
    if (FAILED(m_device->CreateBuffer(&bd, nullptr, &buffer.buffer))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC srvd = {};
    srvd.Format = DXGI_FORMAT_UNKNOWN;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
    srvd.Buffer.FirstElement = 0;
    srvd.Buffer.NumElements = capacity;
    if (FAILED(m_device->CreateShaderResourceView(buffer.buffer.Get(), &srvd, &buffer.srv))) return false;
    buffer.capacity = capacity;
    out = std::move(buffer);
    return true;
}

bool Renderer::EnsureIndexCapacity(uint32_t primCount) {
    if (primCount <= m_indexCapacity) return true;
    if (primCount > kMaxPrims) return false;
    const uint32_t capacity = GrowCapacity(std::max(m_indexCapacity, m_initialCapacity), primCount);
    // Two triangles per record; each index encodes recordIndex * 4 + corner for the vertex shader.
    std::vector<uint32_t> indices(size_t(capacity) * 6);
    for (uint32_t i = 0; i < capacity; ++i) {
        const uint32_t v = i * 4;
        uint32_t* idx = &indices[size_t(i) * 6];
        idx[0] = v;
        idx[1] = v + 1;
        idx[2] = v + 2;
        idx[3] = v;
        idx[4] = v + 2;
        idx[5] = v + 3;
    }
    D3D11_BUFFER_DESC ibd = {};
    ibd.ByteWidth = UINT(indices.size() * sizeof(uint32_t));
    ibd.Usage = D3D11_USAGE_IMMUTABLE;
    ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA init = {indices.data(), 0, 0};
    ComPtr<ID3D11Buffer> indexBuffer;
    if (FAILED(m_device->CreateBuffer(&ibd, &init, &indexBuffer))) return false;
    m_indexBuffer = indexBuffer;
    m_indexCapacity = capacity;
    return true;
}

// =====================================================================================================================
// Recording memory (PrimAllocator)
// =====================================================================================================================
bool Renderer::BeginRecording() {
    // Block 0 is sized for everything the last frame recorded, plus headroom, so a steady frame maps one buffer.
    const uint32_t wanted = std::min(std::max(m_initialCapacity, m_lastRecorded + m_lastRecorded / 4), kMaxPrims);
    if (m_blocks.empty()) m_blocks.emplace_back();
    PrimBuffer& block = m_blocks[0];
    if (block.capacity < wanted && !CreatePrimBuffer(block, GrowCapacity(block.capacity, wanted))) return false;
    // DISCARD: the driver hands out fresh memory while the GPU may still read the previous frame's.
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (FAILED(m_context->Map(block.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    block.mapped = static_cast<GpuPrim*>(mapped.pData);
    block.used = 0;
    m_activeBlocks = 1;
    m_recordSerial = (m_recordSerial + 1) & kSerialMask;
    m_recording = true;
    return true;
}

void Renderer::EndRecording() {
    if (!m_recording) return;
    m_lastRecorded = 0;
    for (uint32_t i = 0; i < m_activeBlocks; ++i) {
        PrimBuffer& block = m_blocks[i];
        if (block.mapped) m_context->Unmap(block.buffer.Get(), 0);
        block.mapped = nullptr;
        m_lastRecorded += block.used;
    }
    m_recording = false;
}

bool Renderer::AllocatePrims(uint32_t minCount, uint32_t desiredCount, PrimChunk& chunk) {
    if (!m_context || minCount == 0 || minCount > kMaxPrims) return false;
    if (!m_recording && !BeginRecording()) return false;
    PrimBuffer* block = &m_blocks[m_activeBlocks - 1];
    if (block->capacity - block->used < minCount) {
        // This frame outgrew its memory: open another block, at least as big as everything recorded so far.
        if (m_activeBlocks == kMaxBlocks) return false;
        uint32_t recorded = 0;
        for (uint32_t i = 0; i < m_activeBlocks; ++i) recorded += m_blocks[i].used;
        if (m_blocks.size() == m_activeBlocks) m_blocks.emplace_back();
        PrimBuffer& next = m_blocks[m_activeBlocks];
        const uint32_t wanted = std::max({minCount, desiredCount, recorded});
        if (next.capacity < wanted && !CreatePrimBuffer(next, GrowCapacity(next.capacity, wanted))) return false;
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(next.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        next.mapped = static_cast<GpuPrim*>(mapped.pData);
        next.used = 0;
        ++m_activeBlocks;
        block = &next;
    }
    const uint32_t count = std::min(block->capacity - block->used, std::max(desiredCount, minCount));
    chunk.data = block->mapped + block->used;
    chunk.count = count;
    chunk.block = BlockId(m_recordSerial, m_activeBlocks - 1);
    chunk.offset = block->used;
    block->used += count;
    return true;
}

const Renderer::PrimBuffer* Renderer::FindBlock(uint32_t block) const {
    // Only the latest recording frame's blocks are drawable: older ones have been reused.
    const uint32_t index = (block & 0xFu) - 1u;
    if ((block >> 4) != m_recordSerial || index >= m_activeBlocks) return nullptr;
    return &m_blocks[index];
}

// =====================================================================================================================
// Rendering
// =====================================================================================================================
void Renderer::Render(const DrawList& list, Vec2 displaySize, const RenderParams& params) {
    const DrawList* lists[] = {&list};
    DrawData data;
    data.lists = lists;
    data.listCount = 1;
    data.displaySize = displaySize;
    Render(data, params);
}

void Renderer::Render(const DrawData& data, const RenderParams& params) {
    m_stats = {};
    EndRecording();
    const Vec2 size = data.displaySize;
    if (!m_context || size.x <= 0.0f || size.y <= 0.0f) return;

    uint32_t total = 0, own = 0;
    for (uint32_t i = 0; i < data.listCount; ++i) {
        total += data.lists[i]->PrimCount();
        own += data.lists[i]->OwnPrimCount();
    }
    if (total == 0) return;

    // Lists that recorded into their own memory are copied with a single map; recorded ones are already in place.
    // DISCARD lets the driver rename the buffer instead of waiting on the GPU.
    if (own > 0) {
        if (own > m_copyBuffer.capacity &&
            (own > kMaxPrims || !CreatePrimBuffer(m_copyBuffer, GrowCapacity(m_copyBuffer.capacity, own)))) {
            return;
        }
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(m_copyBuffer.buffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        auto* dst = static_cast<GpuPrim*>(mapped.pData);
        for (uint32_t i = 0; i < data.listCount; ++i) {
            const DrawList& list = *data.lists[i];
            std::memcpy(dst, list.Prims(), size_t(list.OwnPrimCount()) * sizeof(GpuPrim));
            dst += list.OwnPrimCount();
        }
        m_context->Unmap(m_copyBuffer.buffer.Get(), 0);
    }
    uint32_t indexed = own;
    for (uint32_t i = 0; i < m_activeBlocks; ++i) indexed = std::max(indexed, m_blocks[i].used);
    if (!EnsureIndexCapacity(indexed)) return;

    D3D11_MAPPED_SUBRESOURCE mapped;
    const Constants constants = backend::MakeConstants(size, data.scale, params.colorEncoding, params.paperWhiteNits);
    if (!m_constantsValid || std::memcmp(&constants, m_lastConstants, sizeof(constants)) != 0) {
        if (FAILED(m_context->Map(m_constants.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        m_context->Unmap(m_constants.Get(), 0);
        std::memcpy(m_lastConstants, &constants, sizeof(constants));
        m_constantsValid = true;
    }

    SavedState saved;
    if (params.preserveState) SaveState(saved);

    ID3D11DeviceContext* ctx = m_context.Get();
    const D3D11_VIEWPORT viewport = {0.0f, 0.0f, size.x, size.y, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(m_rasterizer.Get());
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetIndexBuffer(m_indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->VSSetShader(m_vs.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->PSSetShader(m_ps.Get(), nullptr, 0);
    ctx->PSSetConstantBuffers(0, 1, m_constants.GetAddressOf());
    ctx->PSSetSamplers(0, 1, m_sampler.GetAddressOf());
    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ctx->OMSetBlendState(m_blend.Get(), blendFactor, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(m_depthStencil.Get(), 0);

    ID3D11ShaderResourceView* boundPrims = nullptr;
    ID3D11ShaderResourceView* boundTexture = nullptr;
    D3D11_RECT boundScissor = {-1, -1, -1, -1};
    uint32_t base = 0;  // the list's first record in the copy buffer
    for (uint32_t li = 0; li < data.listCount; ++li) {
        const DrawList& list = *data.lists[li];
        const DrawCmd* cmds = list.Cmds();
        for (uint32_t ci = 0; ci < list.CmdCount(); ++ci) {
            const DrawCmd& cmd = cmds[ci];
            if (cmd.primCount == 0) continue;
            ID3D11ShaderResourceView* prims;
            uint32_t first;
            if (cmd.block == 0) {
                prims = m_copyBuffer.srv.Get();
                first = base + cmd.primOffset;
            } else {
                const PrimBuffer* block = FindBlock(cmd.block);
                if (!block) continue;  // recorded before the latest frame: that memory has been reused
                prims = block->srv.Get();
                first = cmd.primOffset;
            }
            const D3D11_RECT scissor = ToScissor(cmd.clipRect, size, data.scale);
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
            if (std::memcmp(&scissor, &boundScissor, sizeof(scissor)) != 0) {
                ctx->RSSetScissorRects(1, &scissor);
                boundScissor = scissor;
            }
            if (prims != boundPrims) {
                ctx->VSSetShaderResources(0, 1, &prims);
                boundPrims = prims;
            }
            ID3D11ShaderResourceView* texture = cmd.texture != kNoTexture
                                                    ? reinterpret_cast<ID3D11ShaderResourceView*>(cmd.texture)
                                                    : m_whiteSrv.Get();
            if (texture != boundTexture) {
                ctx->PSSetShaderResources(1, 1, &texture);
                boundTexture = texture;
            }
            ctx->DrawIndexed(cmd.primCount * 6, first * 6, 0);
            ++m_stats.drawCalls;
            m_stats.prims += cmd.primCount;
        }
        base += list.OwnPrimCount();
    }

    if (params.preserveState) RestoreState(saved);
}

TextureId Renderer::CreateTexture(uint32_t width, uint32_t height, const void* rgba, uint32_t rowPitch) {
    if (!m_device || width == 0 || height == 0 || !rgba) return kNoTexture;
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = width;
    td.Height = height;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA init = {rgba, rowPitch ? rowPitch : width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(m_device->CreateTexture2D(&td, &init, &texture))) return kNoTexture;
    ID3D11ShaderResourceView* srv = nullptr;  // keeps the texture alive; released by DestroyTexture
    if (FAILED(m_device->CreateShaderResourceView(texture.Get(), nullptr, &srv))) return kNoTexture;
    return FromSRV(srv);
}

void Renderer::DestroyTexture(TextureId texture) {
    if (texture != kNoTexture) reinterpret_cast<ID3D11ShaderResourceView*>(texture)->Release();
}

void Renderer::SaveState(SavedState& s) const {
    ID3D11DeviceContext* ctx = m_context.Get();
    ctx->RSGetScissorRects(&s.scissorCount, s.scissors);
    ctx->RSGetViewports(&s.viewportCount, s.viewports);
    ctx->RSGetState(&s.rasterizer);
    ctx->OMGetBlendState(&s.blend, s.blendFactor, &s.sampleMask);
    ctx->OMGetDepthStencilState(&s.depthStencil, &s.stencilRef);
    ctx->VSGetShaderResources(0, 1, &s.vsSrv);
    ctx->PSGetShaderResources(1, 1, &s.psSrv);
    ctx->PSGetSamplers(0, 1, &s.psSampler);
    ctx->VSGetConstantBuffers(0, 1, &s.vsConstants);
    ctx->PSGetConstantBuffers(0, 1, &s.psConstants);
    ctx->VSGetShader(&s.vs, nullptr, nullptr);
    ctx->PSGetShader(&s.ps, nullptr, nullptr);
    ctx->GSGetShader(&s.gs, nullptr, nullptr);
    ctx->HSGetShader(&s.hs, nullptr, nullptr);
    ctx->DSGetShader(&s.ds, nullptr, nullptr);
    ctx->IAGetPrimitiveTopology(&s.topology);
    ctx->IAGetIndexBuffer(&s.indexBuffer, &s.indexFormat, &s.indexOffset);
    ctx->IAGetInputLayout(&s.inputLayout);
}

void Renderer::RestoreState(SavedState& s) const {
    ID3D11DeviceContext* ctx = m_context.Get();
    ctx->RSSetScissorRects(s.scissorCount, s.scissors);
    ctx->RSSetViewports(s.viewportCount, s.viewports);
    ctx->RSSetState(s.rasterizer);
    ctx->OMSetBlendState(s.blend, s.blendFactor, s.sampleMask);
    ctx->OMSetDepthStencilState(s.depthStencil, s.stencilRef);
    ctx->VSSetShaderResources(0, 1, &s.vsSrv);
    ctx->PSSetShaderResources(1, 1, &s.psSrv);
    ctx->PSSetSamplers(0, 1, &s.psSampler);
    ctx->VSSetConstantBuffers(0, 1, &s.vsConstants);
    ctx->PSSetConstantBuffers(0, 1, &s.psConstants);
    ctx->VSSetShader(s.vs, nullptr, 0);
    ctx->PSSetShader(s.ps, nullptr, 0);
    ctx->GSSetShader(s.gs, nullptr, 0);
    ctx->HSSetShader(s.hs, nullptr, 0);
    ctx->DSSetShader(s.ds, nullptr, 0);
    ctx->IASetPrimitiveTopology(s.topology);
    ctx->IASetIndexBuffer(s.indexBuffer, s.indexFormat, s.indexOffset);
    ctx->IASetInputLayout(s.inputLayout);

    SafeRelease(s.rasterizer);
    SafeRelease(s.blend);
    SafeRelease(s.depthStencil);
    SafeRelease(s.vsSrv);
    SafeRelease(s.psSrv);
    SafeRelease(s.psSampler);
    SafeRelease(s.vsConstants);
    SafeRelease(s.psConstants);
    SafeRelease(s.vs);
    SafeRelease(s.ps);
    SafeRelease(s.gs);
    SafeRelease(s.hs);
    SafeRelease(s.ds);
    SafeRelease(s.indexBuffer);
    SafeRelease(s.inputLayout);
}

} // namespace drizzy::d3d11
