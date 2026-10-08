#ifndef NOMINMAX
#define NOMINMAX  // keep <windows.h> (pulled in by d3d12.h) from defining min / max macros
#endif
#include "drizzy/backend_d3d12.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

#include "../backend_common.h"
#include "prim_ps_d3d12.h"  // g_PrimPS12, compiled from src/shaders/prim.hlsl at build time
#include "prim_vs_d3d12.h"  // g_PrimVS12

namespace drizzy::d3d12 {
namespace {

constexpr uint32_t kMaxPrims = 1u << 24;  // see the D3D11 backend
constexpr uint32_t kIndicesPerPrim = 6;
constexpr uint32_t kMaxRecordBlocks = 8;  // per recording frame; see the D3D11 backend
constexpr uint32_t kSerialMask = 0x0FFFFFFFu;

// Block ids: recording frame serial << 4 | block index + 1 (never 0, never 0xFFFFFFFF).
constexpr uint32_t BlockId(uint32_t serial, uint32_t index) { return (serial << 4) | (index + 1); }

uint32_t GrowCapacity(uint32_t current, uint32_t needed) {
    uint32_t capacity = current ? current : 256u;
    while (capacity < needed && capacity < kMaxPrims) capacity *= 2;
    return std::min(capacity, kMaxPrims);
}

// Root signature layout; register assignments match prim.hlsl.
enum RootParam : UINT {
    kRootConstants = 0,  // b0: ndc scale, global flags
    kRootPrims = 1,      // t0: prim buffer (root SRV, no descriptor needed)
    kRootTexture = 2,    // t1: texture descriptor table
};

template <typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

ComPtr<ID3D12Resource> CreateBuffer(ID3D12Device* device, uint64_t size, D3D12_HEAP_TYPE heapType,
                                    D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = heapType;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                               IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

D3D12_RECT ToScissor(const Rect& clip, Vec2 size, float scale) {
    const backend::Scissor s = backend::ToScissor(clip, size, scale);
    return {LONG(s.left), LONG(s.top), LONG(s.right), LONG(s.bottom)};
}

} // namespace

Renderer::~Renderer() { Shutdown(); }

bool Renderer::Init(const RendererDesc& desc) {
    Shutdown();
    if (!desc.device || desc.framesInFlight == 0 || desc.maxTextures == 0) return false;
    m_device = desc.device;
    m_initialCapacity = std::clamp(desc.initialPrimCapacity, 256u, kMaxPrims);

    // Root signature: 4 root constants, a root SRV for the prims, one SRV table for the texture, a static sampler.
    D3D12_DESCRIPTOR_RANGE textureRange = {};
    textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    textureRange.NumDescriptors = 1;
    textureRange.BaseShaderRegister = 1;
    textureRange.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER params[3] = {};
    params[kRootConstants].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[kRootConstants].Constants.ShaderRegister = 0;
    params[kRootConstants].Constants.Num32BitValues = sizeof(backend::Constants) / 4;
    params[kRootConstants].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[kRootPrims].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    params[kRootPrims].Descriptor.ShaderRegister = 0;
    params[kRootPrims].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[kRootTexture].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[kRootTexture].DescriptorTable.NumDescriptorRanges = 1;
    params[kRootTexture].DescriptorTable.pDescriptorRanges = &textureRange;
    params[kRootTexture].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
    rootDesc.NumParameters = 3;
    rootDesc.pParameters = params;
    rootDesc.NumStaticSamplers = 1;
    rootDesc.pStaticSamplers = &sampler;
    rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                     D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(m_device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                             IID_PPV_ARGS(&m_rootSignature)))) {
        Shutdown();
        return false;
    }

    m_sampleCount = std::max(desc.sampleCount, 1u);
    if (!PipelineFor(desc.rtvFormat)) {
        Shutdown();
        return false;
    }

    D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
    heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heapDesc.NumDescriptors = desc.maxTextures;
    heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(m_device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&m_srvHeap)))) {
        Shutdown();
        return false;
    }
    m_srvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_slotResources.resize(desc.maxTextures);
    m_freeSlots.reserve(desc.maxTextures);
    for (uint32_t i = desc.maxTextures; i > 0; --i) m_freeSlots.push_back(i - 1);  // hand out low slots first

    m_frames.resize(desc.framesInFlight);
    m_frameIndex = 0;
    // One more recording set than frames in flight: a set is rewritten only after framesInFlight + 1 Render calls,
    // which the frame contract guarantees the GPU has finished with.
    m_recordSets.resize(desc.framesInFlight + 1);
    m_recordSet = 0;

    m_primHeapType = D3D12_HEAP_TYPE_UPLOAD;
    if (desc.primMemory != PrimMemory::SystemMemory) {
        D3D12_FEATURE_DATA_D3D12_OPTIONS16 options16 = {};
        if (SUCCEEDED(m_device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &options16, sizeof(options16))) &&
            options16.GPUUploadHeapSupported) {
            m_primHeapType = D3D12_HEAP_TYPE_GPU_UPLOAD;
        }
    }

    // Bound for draw commands without a texture so the pixel shader never reads an empty table.
    const uint32_t white = 0xFFFFFFFFu;
    m_whiteTexture = CreateTexture(1, 1, &white);
    if (m_whiteTexture == kNoTexture) {
        Shutdown();
        return false;
    }
    return true;
}

void Renderer::Shutdown() {
    m_primHeapType = D3D12_HEAP_TYPE_UPLOAD;
    m_pendingUploads.clear();
    m_frames.clear();
    m_recordSets.clear();
    m_recording = false;
    m_lastRecorded = 0;
    m_slotResources.clear();
    m_freeSlots.clear();
    m_indexBuffer.Reset();
    m_indexCapacity = 0;
    m_srvHeap.Reset();
    m_pipelines.clear();
    m_rootSignature.Reset();
    m_device.Reset();
    m_whiteTexture = kNoTexture;
}

ID3D12PipelineState* Renderer::PipelineFor(DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_UNKNOWN) return m_pipelines.empty() ? nullptr : m_pipelines[0].state.Get();
    for (const Pipeline& p : m_pipelines) {
        if (p.format == format) return p.state.Get();
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
    pso.pRootSignature = m_rootSignature.Get();
    pso.VS = {g_PrimVS12, sizeof(g_PrimVS12)};
    pso.PS = {g_PrimPS12, sizeof(g_PrimPS12)};
    D3D12_RENDER_TARGET_BLEND_DESC& blend = pso.BlendState.RenderTarget[0];
    blend.BlendEnable = TRUE;  // premultiplied alpha
    blend.SrcBlend = D3D12_BLEND_ONE;
    blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOp = D3D12_BLEND_OP_ADD;
    blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP;
    blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso.SampleMask = UINT_MAX;
    pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso.RasterizerState.DepthClipEnable = TRUE;
    pso.DepthStencilState.DepthEnable = FALSE;
    pso.DepthStencilState.StencilEnable = FALSE;
    pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso.NumRenderTargets = 1;
    pso.RTVFormats[0] = format;
    pso.DSVFormat = DXGI_FORMAT_UNKNOWN;
    pso.SampleDesc.Count = m_sampleCount;
    Pipeline pipeline = {format, nullptr};
    if (FAILED(m_device->CreateGraphicsPipelineState(&pso, IID_PPV_ARGS(&pipeline.state)))) return nullptr;
    m_pipelines.push_back(std::move(pipeline));
    return m_pipelines.back().state.Get();
}

// ---------------------------------------------------------------------------------------------------------------------
// Descriptors and textures
// ---------------------------------------------------------------------------------------------------------------------
D3D12_CPU_DESCRIPTOR_HANDLE Renderer::CpuHandle(uint32_t slot) const {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += size_t(slot) * m_srvIncrement;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::GpuHandle(uint32_t slot) const {
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_srvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += uint64_t(slot) * m_srvIncrement;
    return h;
}

uint32_t Renderer::AllocateSlot() {
    if (m_freeSlots.empty()) return UINT32_MAX;
    const uint32_t slot = m_freeSlots.back();
    m_freeSlots.pop_back();
    return slot;
}

bool Renderer::SlotOf(TextureId texture, uint32_t& slot) const {
    if (!m_srvHeap || texture == kNoTexture) return false;
    const uint64_t start = m_srvHeap->GetGPUDescriptorHandleForHeapStart().ptr;
    if (texture < start || (texture - start) % m_srvIncrement != 0) return false;
    slot = uint32_t((texture - start) / m_srvIncrement);
    return slot < m_slotResources.size() && m_slotResources[slot];
}

TextureId Renderer::CreateTexture(uint32_t width, uint32_t height, const void* rgba, uint32_t rowPitch) {
    if (!m_device || width == 0 || height == 0 || !rgba) return kNoTexture;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> texture;
    if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&texture)))) {
        return kNoTexture;
    }

    PendingUpload upload;
    UINT rows = 0;
    UINT64 rowBytes = 0, totalBytes = 0;
    m_device->GetCopyableFootprints(&desc, 0, 1, 0, &upload.footprint, &rows, &rowBytes, &totalBytes);
    upload.upload = CreateBuffer(m_device.Get(), totalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};
    if (!upload.upload || FAILED(upload.upload->Map(0, &noRead, &mapped))) return kNoTexture;
    const uint32_t srcPitch = rowPitch ? rowPitch : width * 4;
    for (UINT y = 0; y < rows; ++y) {
        std::memcpy(static_cast<uint8_t*>(mapped) + upload.footprint.Offset + size_t(y) * upload.footprint.Footprint.RowPitch,
                    static_cast<const uint8_t*>(rgba) + size_t(y) * srcPitch, size_t(width) * 4);
    }
    upload.upload->Unmap(0, nullptr);

    const uint32_t slot = AllocateSlot();
    if (slot == UINT32_MAX) return kNoTexture;
    m_device->CreateShaderResourceView(texture.Get(), nullptr, CpuHandle(slot));
    m_slotResources[slot] = texture;
    upload.texture = texture;
    m_pendingUploads.push_back(std::move(upload));
    return GpuHandle(slot).ptr;
}

TextureId Renderer::RegisterTexture(ID3D12Resource* resource, const D3D12_SHADER_RESOURCE_VIEW_DESC* srvDesc) {
    if (!m_device || !resource) return kNoTexture;
    const uint32_t slot = AllocateSlot();
    if (slot == UINT32_MAX) return kNoTexture;
    m_device->CreateShaderResourceView(resource, srvDesc, CpuHandle(slot));
    m_slotResources[slot] = resource;
    return GpuHandle(slot).ptr;
}

void Renderer::DestroyTexture(TextureId texture) {
    uint32_t slot;
    if (!SlotOf(texture, slot) || m_frames.empty()) return;
    ID3D12Resource* resource = m_slotResources[slot].Get();
    m_pendingUploads.erase(std::remove_if(m_pendingUploads.begin(), m_pendingUploads.end(),
                                          [resource](const PendingUpload& u) { return u.texture.Get() == resource; }),
                           m_pendingUploads.end());
    // Frames already recorded may still sample it: retire it with the current frame's buffers.
    Frame& frame = m_frames[m_frameIndex];
    frame.garbage.push_back(std::move(m_slotResources[slot]));
    frame.freedSlots.push_back(slot);
}

// ---------------------------------------------------------------------------------------------------------------------
// Per-frame resources
// ---------------------------------------------------------------------------------------------------------------------
void Renderer::RecycleFrame(Frame& frame) {
    frame.garbage.clear();
    m_freeSlots.insert(m_freeSlots.end(), frame.freedSlots.begin(), frame.freedSlots.end());
    frame.freedSlots.clear();
}

void Renderer::FlushUploads(ID3D12GraphicsCommandList* cmd, Frame& frame) {
    if (m_pendingUploads.empty()) return;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    barriers.reserve(m_pendingUploads.size());
    for (PendingUpload& upload : m_pendingUploads) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = upload.texture.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = upload.upload.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint = upload.footprint;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barriers.push_back(Transition(upload.texture.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE));
        frame.garbage.push_back(std::move(upload.upload));  // the copy reads it until this frame completes
    }
    cmd->ResourceBarrier(UINT(barriers.size()), barriers.data());
    m_pendingUploads.clear();
}

// A persistently mapped buffer of `capacity` records in the prim heap.
Renderer::ComPtr<ID3D12Resource> Renderer::CreatePrimBuffer(uint32_t capacity, GpuPrim*& mapped) {
    mapped = nullptr;
    const uint64_t bytes = uint64_t(capacity) * sizeof(GpuPrim);
    ComPtr<ID3D12Resource> buffer;
    if (m_primHeapType == D3D12_HEAP_TYPE_GPU_UPLOAD) {
        // GPU upload heap buffers start in COMMON and are promoted to a shader-read state on use.
        buffer = CreateBuffer(m_device.Get(), bytes, m_primHeapType, D3D12_RESOURCE_STATE_COMMON);
        if (!buffer) m_primHeapType = D3D12_HEAP_TYPE_UPLOAD;  // e.g. the BAR window is exhausted
    }
    if (!buffer) buffer = CreateBuffer(m_device.Get(), bytes, m_primHeapType, D3D12_RESOURCE_STATE_GENERIC_READ);
    const D3D12_RANGE noRead = {0, 0};
    void* data = nullptr;
    if (!buffer || FAILED(buffer->Map(0, &noRead, &data))) return nullptr;
    mapped = static_cast<GpuPrim*>(data);
    return buffer;
}

bool Renderer::EnsurePrimCapacity(Frame& frame, uint32_t primCount) {
    if (primCount <= frame.capacity) return true;
    if (primCount > kMaxPrims) return false;
    const uint32_t capacity = GrowCapacity(std::max(frame.capacity, m_initialCapacity), primCount);
    // This frame's previous GPU use has finished (frame contract), so the old buffer can go right away.
    frame.primBuffer = CreatePrimBuffer(capacity, frame.mapped);
    frame.capacity = frame.primBuffer ? capacity : 0;
    return frame.primBuffer != nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Recording memory (PrimAllocator)
// ---------------------------------------------------------------------------------------------------------------------
bool Renderer::BeginRecording() {
    if (m_recordSets.empty()) return false;
    m_recordSet = (m_recordSet + 1) % uint32_t(m_recordSets.size());
    m_recordSerial = (m_recordSerial + 1) & kSerialMask;
    RecordSet& set = m_recordSets[m_recordSet];
    set.activeBlocks = 0;
    // Block 0 is sized for everything the last frame recorded, plus headroom, so a steady frame uses one buffer. The
    // set was last drawn framesInFlight + 1 Render calls ago, so its buffers can be replaced right away.
    const uint32_t wanted = std::min(std::max(m_initialCapacity, m_lastRecorded + m_lastRecorded / 4), kMaxPrims);
    if (set.blocks.empty()) set.blocks.emplace_back();
    RecordBlock& block = set.blocks[0];
    if (block.capacity < wanted) {
        const uint32_t capacity = GrowCapacity(block.capacity, wanted);
        block.buffer = CreatePrimBuffer(capacity, block.mapped);
        block.capacity = block.buffer ? capacity : 0;
        if (!block.buffer) return false;
    }
    block.used = 0;
    set.activeBlocks = 1;
    m_recording = true;
    return true;
}

void Renderer::EndRecording() {
    if (!m_recording) return;
    const RecordSet& set = m_recordSets[m_recordSet];
    m_lastRecorded = 0;
    for (uint32_t i = 0; i < set.activeBlocks; ++i) m_lastRecorded += set.blocks[i].used;
    m_recording = false;
}

bool Renderer::AllocatePrims(uint32_t minCount, uint32_t desiredCount, PrimChunk& chunk) {
    if (!m_device || minCount == 0 || minCount > kMaxPrims) return false;
    if (!m_recording && !BeginRecording()) return false;
    RecordSet& set = m_recordSets[m_recordSet];
    RecordBlock* block = &set.blocks[set.activeBlocks - 1];
    if (block->capacity - block->used < minCount) {
        // This frame outgrew its memory: open another block, at least as big as everything recorded so far.
        if (set.activeBlocks == kMaxRecordBlocks) return false;
        uint32_t recorded = 0;
        for (uint32_t i = 0; i < set.activeBlocks; ++i) recorded += set.blocks[i].used;
        if (set.blocks.size() == set.activeBlocks) set.blocks.emplace_back();
        RecordBlock& next = set.blocks[set.activeBlocks];
        const uint32_t wanted = std::max({minCount, desiredCount, recorded});
        if (next.capacity < wanted) {
            const uint32_t capacity = GrowCapacity(next.capacity, wanted);
            next.buffer = CreatePrimBuffer(capacity, next.mapped);
            next.capacity = next.buffer ? capacity : 0;
            if (!next.buffer) return false;
        }
        next.used = 0;
        ++set.activeBlocks;
        block = &next;
    }
    const uint32_t count = std::min(block->capacity - block->used, std::max(desiredCount, minCount));
    chunk.data = block->mapped + block->used;
    chunk.count = count;
    chunk.block = BlockId(m_recordSerial, set.activeBlocks - 1);
    chunk.offset = block->used;
    block->used += count;
    return true;
}

const Renderer::RecordBlock* Renderer::FindBlock(uint32_t block) const {
    // Only the latest recording frame's blocks are drawable: older sets are being reused.
    if (m_recordSets.empty() || (block >> 4) != m_recordSerial) return nullptr;
    const RecordSet& set = m_recordSets[m_recordSet];
    const uint32_t index = (block & 0xFu) - 1u;
    return index < set.activeBlocks ? &set.blocks[index] : nullptr;
}

bool Renderer::EnsureIndexCapacity(ID3D12GraphicsCommandList* cmd, Frame& frame, uint32_t primCount) {
    if (primCount <= m_indexCapacity) return true;
    if (primCount > kMaxPrims) return false;
    uint32_t capacity = std::max(m_indexCapacity, m_initialCapacity);
    while (capacity < primCount) capacity *= 2;
    capacity = std::min(capacity, kMaxPrims);

    // Static index buffer in video memory: two triangles per prim, each index = primIndex * 4 + corner.
    const uint64_t bytes = uint64_t(capacity) * kIndicesPerPrim * sizeof(uint32_t);
    ComPtr<ID3D12Resource> indexBuffer =
        CreateBuffer(m_device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    ComPtr<ID3D12Resource> upload =
        CreateBuffer(m_device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* mapped = nullptr;
    const D3D12_RANGE noRead = {0, 0};
    if (!indexBuffer || !upload || FAILED(upload->Map(0, &noRead, &mapped))) return false;
    auto* idx = static_cast<uint32_t*>(mapped);
    for (uint32_t i = 0; i < capacity; ++i, idx += kIndicesPerPrim) {
        const uint32_t v = i * 4;
        idx[0] = v;
        idx[1] = v + 1;
        idx[2] = v + 2;
        idx[3] = v;
        idx[4] = v + 2;
        idx[5] = v + 3;
    }
    upload->Unmap(0, nullptr);

    D3D12_RESOURCE_BARRIER toCopy =
        Transition(indexBuffer.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->ResourceBarrier(1, &toCopy);
    cmd->CopyBufferRegion(indexBuffer.Get(), 0, upload.Get(), 0, bytes);
    D3D12_RESOURCE_BARRIER toIndex =
        Transition(indexBuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    cmd->ResourceBarrier(1, &toIndex);

    // Earlier frames may still be drawing with the old index buffer.
    if (m_indexBuffer) frame.garbage.push_back(std::move(m_indexBuffer));
    frame.garbage.push_back(std::move(upload));
    m_indexBuffer = std::move(indexBuffer);
    m_indexCapacity = capacity;
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------------------------------
void Renderer::Render(ID3D12GraphicsCommandList* cmd, const DrawList& list, Vec2 displaySize,
                      const RenderParams& params) {
    const DrawList* lists[] = {&list};
    DrawData data;
    data.lists = lists;
    data.listCount = 1;
    data.displaySize = displaySize;
    Render(cmd, data, params);
}

void Renderer::Render(ID3D12GraphicsCommandList* cmd, const DrawData& data, const RenderParams& params) {
    m_stats = {};
    EndRecording();
    if (!m_device || !cmd) return;

    m_frameIndex = (m_frameIndex + 1) % uint32_t(m_frames.size());
    Frame& frame = m_frames[m_frameIndex];
    RecycleFrame(frame);
    FlushUploads(cmd, frame);

    const Vec2 size = data.displaySize;
    if (size.x <= 0.0f || size.y <= 0.0f) return;
    uint32_t total = 0, own = 0;
    for (uint32_t i = 0; i < data.listCount; ++i) {
        total += data.lists[i]->PrimCount();
        own += data.lists[i]->OwnPrimCount();
    }
    if (total == 0) return;
    uint32_t indexed = own;
    if (!m_recordSets.empty()) {
        const RecordSet& set = m_recordSets[m_recordSet];
        for (uint32_t i = 0; i < set.activeBlocks; ++i) indexed = std::max(indexed, set.blocks[i].used);
    }
    if ((own > 0 && !EnsurePrimCapacity(frame, own)) || !EnsureIndexCapacity(cmd, frame, indexed)) return;

    // Lists that recorded into their own memory are copied; recorded ones are already where the GPU reads them.
    GpuPrim* dst = frame.mapped;
    for (uint32_t i = 0; i < data.listCount && own > 0; ++i) {
        const DrawList& list = *data.lists[i];
        std::memcpy(dst, list.Prims(), size_t(list.OwnPrimCount()) * sizeof(GpuPrim));
        dst += list.OwnPrimCount();
    }

    cmd->SetGraphicsRootSignature(m_rootSignature.Get());
    ID3D12PipelineState* pipeline = PipelineFor(params.rtvFormat);
    if (!pipeline) return;
    cmd->SetPipelineState(pipeline);
    ID3D12DescriptorHeap* heaps[] = {m_srvHeap.Get()};
    cmd->SetDescriptorHeaps(1, heaps);
    if (params.rtv.ptr) cmd->OMSetRenderTargets(1, &params.rtv, FALSE, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_INDEX_BUFFER_VIEW ibv = {m_indexBuffer->GetGPUVirtualAddress(),
                                         m_indexCapacity * kIndicesPerPrim * UINT(sizeof(uint32_t)),
                                         DXGI_FORMAT_R32_UINT};
    cmd->IASetIndexBuffer(&ibv);
    const D3D12_VIEWPORT viewport = {0.0f, 0.0f, size.x, size.y, 0.0f, 1.0f};
    cmd->RSSetViewports(1, &viewport);
    const backend::Constants constants =
        backend::MakeConstants(size, data.scale, params.colorEncoding, params.paperWhiteNits);
    cmd->SetGraphicsRoot32BitConstants(kRootConstants, sizeof(constants) / 4, &constants, 0);

    D3D12_GPU_VIRTUAL_ADDRESS boundPrims = 0;
    TextureId boundTexture = kNoTexture;
    D3D12_RECT boundScissor = {-1, -1, -1, -1};
    uint32_t base = 0;  // the list's first record in the copy buffer
    for (uint32_t li = 0; li < data.listCount; ++li) {
        const DrawList& list = *data.lists[li];
        const DrawCmd* cmds = list.Cmds();
        for (uint32_t ci = 0; ci < list.CmdCount(); ++ci) {
            const DrawCmd& dc = cmds[ci];
            if (dc.primCount == 0) continue;
            D3D12_GPU_VIRTUAL_ADDRESS prims;
            uint32_t first;
            if (dc.block == 0) {
                prims = frame.primBuffer->GetGPUVirtualAddress();
                first = base + dc.primOffset;
            } else {
                const RecordBlock* block = FindBlock(dc.block);
                if (!block) continue;  // recorded before the latest frame: that memory is being reused
                prims = block->buffer->GetGPUVirtualAddress();
                first = dc.primOffset;
            }
            const D3D12_RECT scissor = ToScissor(dc.clipRect, size, data.scale);
            if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
            if (std::memcmp(&scissor, &boundScissor, sizeof(scissor)) != 0) {
                cmd->RSSetScissorRects(1, &scissor);
                boundScissor = scissor;
            }
            if (prims != boundPrims) {
                cmd->SetGraphicsRootShaderResourceView(kRootPrims, prims);
                boundPrims = prims;
            }
            const TextureId texture = dc.texture != kNoTexture ? dc.texture : m_whiteTexture;
            if (texture != boundTexture) {
                cmd->SetGraphicsRootDescriptorTable(kRootTexture, D3D12_GPU_DESCRIPTOR_HANDLE{texture});
                boundTexture = texture;
            }
            cmd->DrawIndexedInstanced(dc.primCount * kIndicesPerPrim, 1, first * kIndicesPerPrim, 0, 0);
            ++m_stats.drawCalls;
            m_stats.prims += dc.primCount;
        }
        base += list.OwnPrimCount();
    }
}

} // namespace drizzy::d3d12
