#include "host.h"

#include "drizzy/backend_d3d12.h"
#include "png_writer.h"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace drizzy;

namespace sandbox {
namespace {

constexpr uint32_t kFramesInFlight = 2;
constexpr uint32_t kBackBuffers = 3;
constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> CreateReadbackBuffer(ID3D12Device* device, uint64_t size) {
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&buffer)))) {
        return nullptr;
    }
    return buffer;
}

class D3D12Host final : public GpuHost {
public:
    explicit D3D12Host(PrimMemoryChoice primMemory) : m_primMemory(primMemory) {}
    ~D3D12Host() override {
        if (m_queue && m_fence) WaitIdle();
        m_renderer.Shutdown();
        if (m_fenceEvent) CloseHandle(m_fenceEvent);
    }

    const char* ApiName() const override { return "D3D12"; }

    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool warp) override {
#if defined(_DEBUG)
        ComPtr<ID3D12Debug> debug;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
#endif
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&m_factory)))) return false;
        ComPtr<IDXGIAdapter1> adapter;
        if (warp) {
            m_factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
        } else {
            ComPtr<IDXGIFactory6> factory6;
            if (SUCCEEDED(m_factory.As(&factory6))) {
                factory6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
            }
            if (!adapter) m_factory->EnumAdapters1(0, &adapter);
        }
        if (!adapter || FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_device)))) {
            return false;
        }
        DXGI_ADAPTER_DESC1 adapterDesc = {};
        adapter->GetDesc1(&adapterDesc);
        m_adapterName = adapterDesc.Description;

        D3D12_COMMAND_QUEUE_DESC queueDesc = {};
        queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (FAILED(m_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_queue)))) return false;
        for (FrameContext& frame : m_frames) {
            if (FAILED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                        IID_PPV_ARGS(&frame.allocator)))) {
                return false;
            }
        }
        if (FAILED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(), nullptr,
                                               IID_PPV_ARGS(&m_list))) ||
            FAILED(m_list->Close()) ||
            FAILED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)))) {
            return false;
        }
        m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_fenceEvent) return false;

        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = kBackBuffers + 1;  // swap chain buffers + the offscreen target
        if (FAILED(m_device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap)))) return false;
        m_rtvIncrement = m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        m_width = width;
        m_height = height;
        if (hwnd) {
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            if (SUCCEEDED(m_factory.As(&factory5))) {
                factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
            }
            m_tearing = tearing != FALSE;
            DXGI_SWAP_CHAIN_DESC1 scd = {};
            scd.Width = width;
            scd.Height = height;
            scd.Format = kFormat;
            scd.SampleDesc.Count = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.BufferCount = kBackBuffers;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            ComPtr<IDXGISwapChain1> swapChain;
            if (FAILED(m_factory->CreateSwapChainForHwnd(m_queue.Get(), hwnd, &scd, nullptr, nullptr, &swapChain)) ||
                FAILED(swapChain.As(&m_swapChain))) {
                return false;
            }
            m_factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (!CreateBackBufferViews()) return false;
        } else {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = kFormat;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear = {};
            clear.Format = kFormat;
            std::memcpy(clear.Color, kClearColor, sizeof(kClearColor));
            if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                                                         IID_PPV_ARGS(&m_offscreen)))) {
                return false;
            }
            m_device->CreateRenderTargetView(m_offscreen.Get(), nullptr, RtvHandle(kBackBuffers));
        }

        D3D12_QUERY_HEAP_DESC queryDesc = {};
        queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        queryDesc.Count = 2 * kFramesInFlight;
        if (FAILED(m_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_queryHeap)))) return false;
        m_queryReadback = CreateReadbackBuffer(m_device.Get(), sizeof(uint64_t) * 2 * kFramesInFlight);
        if (!m_queryReadback || FAILED(m_queue->GetTimestampFrequency(&m_timestampFrequency))) return false;

        d3d12::RendererDesc rendererDesc;
        rendererDesc.device = m_device.Get();
        rendererDesc.framesInFlight = kFramesInFlight;
        rendererDesc.rtvFormat = kFormat;
        rendererDesc.primMemory = m_primMemory == PrimMemoryChoice::Video    ? d3d12::PrimMemory::VideoMemory
                                : m_primMemory == PrimMemoryChoice::System ? d3d12::PrimMemory::SystemMemory
                                                                           : d3d12::PrimMemory::Auto;
        return m_renderer.Init(rendererDesc);
    }

    bool Resize(uint32_t width, uint32_t height) override {
        if (!m_swapChain) return false;
        WaitIdle();
        for (auto& buffer : m_backBuffers) buffer.Reset();
        DXGI_SWAP_CHAIN_DESC1 desc;
        m_swapChain->GetDesc1(&desc);
        if (FAILED(m_swapChain->ResizeBuffers(kBackBuffers, width, height, DXGI_FORMAT_UNKNOWN, desc.Flags))) return false;
        m_width = width;
        m_height = height;
        return CreateBackBufferViews();
    }

    TextureId CreateTexture(uint32_t width, uint32_t height, const void* rgba) override {
        return m_renderer.CreateTexture(width, height, rgba);
    }
    void DestroyTexture(TextureId texture) override { m_renderer.DestroyTexture(texture); }

    double RenderFrame(const DrawData& data, bool vsync) override {
        const uint32_t slot = uint32_t(m_frameNumber % kFramesInFlight);
        FrameContext& frame = m_frames[slot];
        // Waiting for this slot's previous frame is also what the renderer's frame contract asks for.
        WaitForFence(frame.fenceValue);
        if (frame.timestampPending) {
            double ms;
            if (ReadTimestamp(slot, ms)) m_ready.push_back(ms);
            m_pending.erase(std::find(m_pending.begin(), m_pending.end(), slot));
        }
        frame.allocator->Reset();
        m_list->Reset(frame.allocator.Get(), nullptr);

        ID3D12Resource* target;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv;
        if (m_swapChain) {
            const UINT index = m_swapChain->GetCurrentBackBufferIndex();
            target = m_backBuffers[index].Get();
            rtv = RtvHandle(index);
            Transition(m_list.Get(), target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        } else {
            target = m_offscreen.Get();
            rtv = RtvHandle(kBackBuffers);
        }
        m_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        m_list->ClearRenderTargetView(rtv, kClearColor, 0, nullptr);

        m_list->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
        d3d12::RenderParams params;
        params.rtv = rtv;
        const int64_t t0 = Now();
        m_renderer.Render(m_list.Get(), data, params);
        const int64_t t1 = Now();
        m_list->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
        m_list->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, m_queryReadback.Get(),
                                 uint64_t(slot) * 2 * sizeof(uint64_t));

        if (m_swapChain) {
            Transition(m_list.Get(), target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        }
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        if (m_swapChain) m_swapChain->Present(vsync ? 1 : 0, (!vsync && m_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        frame.fenceValue = ++m_fenceValue;
        m_queue->Signal(m_fence.Get(), frame.fenceValue);
        frame.timestampPending = true;
        m_pending.push_back(slot);
        ++m_frameNumber;
        return TicksToMs(t1 - t0);
    }

    bool PollGpuTime(double& ms) override {
        if (m_ready.empty() && !m_pending.empty()) {
            const uint32_t slot = m_pending.front();
            if (m_fence->GetCompletedValue() >= m_frames[slot].fenceValue) {
                m_pending.pop_front();
                double value;
                if (ReadTimestamp(slot, value)) m_ready.push_back(value);
            }
        }
        if (m_ready.empty()) return false;
        ms = m_ready.front();
        m_ready.pop_front();
        return true;
    }

    bool WaitGpuTime(double& ms) override {
        if (m_pending.empty()) return false;
        const uint32_t slot = m_pending.back();
        WaitForFence(m_frames[slot].fenceValue);
        for (uint32_t s : m_pending) m_frames[s].timestampPending = false;
        m_pending.clear();
        m_ready.clear();
        return ReadTimestamp(slot, ms);
    }

    bool SavePng(const char* path) override {
        if (!m_offscreen) return false;
        WaitIdle();
        FrameContext& frame = m_frames[m_frameNumber % kFramesInFlight];
        frame.allocator->Reset();
        m_list->Reset(frame.allocator.Get(), nullptr);

        const D3D12_RESOURCE_DESC desc = m_offscreen->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
        UINT rows = 0;
        UINT64 rowBytes = 0, totalBytes = 0;
        m_device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &totalBytes);
        ComPtr<ID3D12Resource> readback = CreateReadbackBuffer(m_device.Get(), totalBytes);
        if (!readback) return false;

        Transition(m_list.Get(), m_offscreen.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = readback.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = m_offscreen.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        m_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Transition(m_list.Get(), m_offscreen.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_list->Close();
        ID3D12CommandList* lists[] = {m_list.Get()};
        m_queue->ExecuteCommandLists(1, lists);
        WaitIdle();

        void* mapped = nullptr;
        const D3D12_RANGE readRange = {0, size_t(totalBytes)};
        if (FAILED(readback->Map(0, &readRange, &mapped))) return false;
        const bool ok = WriteOpaquePng(path, m_width, m_height, static_cast<const uint8_t*>(mapped) + footprint.Offset,
                                       footprint.Footprint.RowPitch);
        const D3D12_RANGE noWrite = {0, 0};
        readback->Unmap(0, &noWrite);
        return ok;
    }

    RenderStats Stats() const override { return m_renderer.LastFrameStats(); }
    PrimAllocator* Allocator() override { return &m_renderer; }
    std::wstring AdapterName() const override { return m_adapterName; }
    const char* Details() const override {
        return m_renderer.PrimsInVideoMemory() ? "prims written to video memory (GPU upload heap)"
                                               : "prims read from system memory (upload heap)";
    }

    int ReportDebugMessages() override {
        ComPtr<ID3D12InfoQueue> queue;
        if (FAILED(m_device.As(&queue))) return -1;
        int problems = 0;
        const UINT64 count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            queue->GetMessage(i, nullptr, &length);
            std::vector<char> storage(length);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(queue->GetMessage(i, message, &length))) continue;
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D12: %.*s\n", int(message->DescriptionByteLength), message->pDescription);
                ++problems;
            }
        }
        queue->ClearStoredMessages();
        return problems;
    }

private:
    struct FrameContext {
        ComPtr<ID3D12CommandAllocator> allocator;
        UINT64 fenceValue = 0;
        bool timestampPending = false;
    };

    D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(uint32_t index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(index) * m_rtvIncrement;
        return h;
    }

    bool CreateBackBufferViews() {
        for (uint32_t i = 0; i < kBackBuffers; ++i) {
            if (FAILED(m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i])))) return false;
            m_device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, RtvHandle(i));
        }
        return true;
    }

    void WaitForFence(UINT64 value) {
        if (m_fence->GetCompletedValue() >= value) return;
        m_fence->SetEventOnCompletion(value, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }

    void WaitIdle() {
        m_queue->Signal(m_fence.Get(), ++m_fenceValue);
        WaitForFence(m_fenceValue);
    }

    bool ReadTimestamp(uint32_t slot, double& ms) {
        m_frames[slot].timestampPending = false;
        const size_t offset = size_t(slot) * 2 * sizeof(uint64_t);
        const D3D12_RANGE range = {offset, offset + 2 * sizeof(uint64_t)};
        void* mapped = nullptr;
        if (FAILED(m_queryReadback->Map(0, &range, &mapped))) return false;
        const auto* ticks = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + offset);
        const uint64_t t0 = ticks[0], t1 = ticks[1];
        const D3D12_RANGE noWrite = {0, 0};
        m_queryReadback->Unmap(0, &noWrite);
        if (t1 < t0 || m_timestampFrequency == 0) return false;
        ms = double(t1 - t0) * 1000.0 / double(m_timestampFrequency);
        return true;
    }

    ComPtr<IDXGIFactory4> m_factory;
    ComPtr<ID3D12Device> m_device;
    ComPtr<ID3D12CommandQueue> m_queue;
    ComPtr<ID3D12GraphicsCommandList> m_list;
    ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr;
    UINT64 m_fenceValue = 0;
    FrameContext m_frames[kFramesInFlight];
    uint64_t m_frameNumber = 0;
    ComPtr<IDXGISwapChain3> m_swapChain;
    ComPtr<ID3D12Resource> m_backBuffers[kBackBuffers];
    ComPtr<ID3D12Resource> m_offscreen;
    ComPtr<ID3D12DescriptorHeap> m_rtvHeap;
    UINT m_rtvIncrement = 0;
    bool m_tearing = false;
    uint32_t m_width = 0, m_height = 0;
    ComPtr<ID3D12QueryHeap> m_queryHeap;
    ComPtr<ID3D12Resource> m_queryReadback;
    UINT64 m_timestampFrequency = 0;
    std::deque<uint32_t> m_pending;  // frame slots with unread timestamps, oldest first
    std::deque<double> m_ready;      // measurements read early (when a slot was reused)
    std::wstring m_adapterName;
    PrimMemoryChoice m_primMemory = PrimMemoryChoice::Auto;
    d3d12::Renderer m_renderer;
};

} // namespace

std::unique_ptr<GpuHost> CreateD3D12Host(PrimMemoryChoice primMemory) {
    return std::make_unique<D3D12Host>(primMemory);
}

} // namespace sandbox
