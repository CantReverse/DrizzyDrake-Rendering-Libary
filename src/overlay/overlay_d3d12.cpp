#include "drizzy/overlay_d3d12.h"

#include <dxgi1_4.h>

#include <chrono>

namespace drizzy::overlay {
namespace {

constexpr uint32_t kFenceTimeoutMs = 2000;  // never stall the game for longer than this on our own work

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

float MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

// =====================================================================================================================
// D3D12QueueCapture
// =====================================================================================================================
void D3D12QueueCapture::OnExecuteCommandLists(ID3D12CommandQueue* queue) {
    if (!queue) return;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;  // the present queue is always DIRECT
    if (m_device) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) && device.Get() != m_device) return;
    }
    Lock();
    Entry* entry = nullptr;
    for (uint32_t i = 0; i < m_entryCount && !entry; ++i) {
        if (m_entries[i].queue == queue) entry = &m_entries[i];
    }
    if (!entry && m_entryCount < kMaxQueues) {
        queue->AddRef();  // released once the queue stops submitting (OnPresent)
        entry = &m_entries[m_entryCount++];
        *entry = {queue, 0, m_window};
    }
    if (entry) {
        ++entry->count;
        entry->lastSeen = m_window;
    }
    ID3D12CommandQueue* best = nullptr;
    uint32_t bestCount = 0;
    for (uint32_t i = 0; i < m_entryCount; ++i) {
        if (m_entries[i].count > bestCount) {
            bestCount = m_entries[i].count;
            best = m_entries[i].queue;
        }
    }
    m_best.store(best, std::memory_order_release);
    Unlock();
}

void D3D12QueueCapture::OnPresent() {
    Lock();
    if (++m_presents % kWindowFrames == 0) {
        ++m_window;
        // Drop queues that submitted nothing for two windows (destroyed, or idle); halve the others' counts so the
        // choice follows what the game does now without flipping between queues at every window.
        ID3D12CommandQueue* best = nullptr;
        uint32_t bestCount = 0;
        for (uint32_t i = 0; i < m_entryCount;) {
            Entry& e = m_entries[i];
            if (m_window - e.lastSeen > 2) {
                e.queue->Release();
                e = m_entries[--m_entryCount];
                m_entries[m_entryCount] = {};
                continue;
            }
            e.count = e.count / 2 + (e.count & 1);
            if (e.count > bestCount) {
                bestCount = e.count;
                best = e.queue;
            }
            ++i;
        }
        m_best.store(best, std::memory_order_release);
    }
    Unlock();
}

void D3D12QueueCapture::Reset() {
    Lock();
    for (uint32_t i = 0; i < m_entryCount; ++i) m_entries[i].queue->Release();
    for (Entry& entry : m_entries) entry = {};
    m_entryCount = 0;
    m_best.store(nullptr, std::memory_order_release);
    Unlock();
}

// =====================================================================================================================
// D3D12Overlay
// =====================================================================================================================
D3D12Overlay::~D3D12Overlay() {
    Shutdown();
    m_queue.Reset();
}

void D3D12Overlay::SetCommandQueue(ID3D12CommandQueue* queue) {
    if (!queue || queue == m_queue.Get()) return;
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
    if (m_device && device == m_device) {
        // Another queue of the same device: finish our work on the old one and carry on - no rebuild.
        WaitForGpu();
        m_queue = queue;
        UINT64 frequency = 0;
        if (SUCCEEDED(m_queue->GetTimestampFrequency(&frequency)) && frequency) m_timestampToMs = 1000.0 / double(frequency);
        return;
    }
    // A queue of another device: everything is rebuilt on it.
    Shutdown();
    m_queue = queue;
    m_removed = false;
}

bool D3D12Overlay::WaitForFence(uint64_t value, uint32_t timeoutMs) {
    if (!m_fence || m_fence->GetCompletedValue() >= value) return true;
    if (FAILED(m_fence->SetEventOnCompletion(value, m_fenceEvent))) return false;
    return WaitForSingleObject(m_fenceEvent, timeoutMs) == WAIT_OBJECT_0;
}

void D3D12Overlay::WaitForGpu() {
    if (!m_queue || !m_fence || !m_fenceEvent) return;
    const uint64_t value = ++m_fenceValue;
    if (SUCCEEDED(m_queue->Signal(m_fence.Get(), value))) WaitForFence(value, kFenceTimeoutMs);
}

void D3D12Overlay::OnResizeBuffers() {
    WaitForGpu();
    m_formatSeen = DXGI_FORMAT_UNKNOWN;
}

void D3D12Overlay::SetColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (colorSpace == m_colorSpace) return;
    m_colorSpace = colorSpace;
    m_formatSeen = DXGI_FORMAT_UNKNOWN;  // detect the encoding again
}

void D3D12Overlay::Shutdown() {
    WaitForGpu();
    m_renderer.Shutdown();
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    for (FrameSlot& slot : m_frames) slot = {};
    m_queryReadback.Reset();
    m_queryHeap.Reset();
    m_fence.Reset();
    m_rtvHeap.Reset();
    m_list.Reset();
    m_device.Reset();
    // m_queue is kept: the caller may Render again with the same queue.
    m_fenceValue = 0;
    m_frameCounter = 0;
    m_formatSeen = DXGI_FORMAT_UNKNOWN;
    m_stats = {};
}

bool D3D12Overlay::Initialize(IDXGISwapChain* swapChain) { return EnsureSetup(swapChain); }

bool D3D12Overlay::EnsureSetup(IDXGISwapChain* swapChain) {
    if (!swapChain) return false;
    if (swapChain != m_swapChain) {
        m_swapChain = swapChain;
        m_formatSeen = DXGI_FORMAT_UNKNOWN;
        // A D3D12 swap chain's "device" is the queue it presents with; when the runtime hands it out, no queue capture
        // is needed.
        Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
        if (SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&queue)))) SetCommandQueue(queue.Get());
    }
    if (!m_queue || m_removed) return false;
    if (m_device) {
        if (m_device->GetDeviceRemovedReason() == S_OK) return true;
        // The device is gone (driver reset, GPU removed): stop until the game brings up a new queue.
        Shutdown();
        m_removed = true;
        return false;
    }

    if (FAILED(m_queue->GetDevice(IID_PPV_ARGS(&m_device))) || m_device->GetDeviceRemovedReason() != S_OK) {
        m_device.Reset();
        return false;
    }
    DXGI_SWAP_CHAIN_DESC scDesc;
    if (FAILED(swapChain->GetDesc(&scDesc))) {
        m_device.Reset();
        return false;
    }

    bool ok = true;
    for (FrameSlot& slot : m_frames) {
        ok = ok && SUCCEEDED(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                              IID_PPV_ARGS(&slot.allocator)));
    }
    ok = ok && SUCCEEDED(m_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_frames[0].allocator.Get(),
                                                     nullptr, IID_PPV_ARGS(&m_list))) &&
         SUCCEEDED(m_list->Close()) &&
         SUCCEEDED(m_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence)));
    // One RTV is enough: render target descriptors are copied into the command list when recorded.
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc = {};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    ok = ok && SUCCEEDED(m_device->CreateDescriptorHeap(&rtvDesc, IID_PPV_ARGS(&m_rtvHeap)));
    // GPU timing: a pair of timestamps per frame slot, read back when the slot comes around again.
    D3D12_QUERY_HEAP_DESC queryDesc = {};
    queryDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    queryDesc.Count = 2 * kFrames;
    if (ok && SUCCEEDED(m_device->CreateQueryHeap(&queryDesc, IID_PPV_ARGS(&m_queryHeap)))) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = sizeof(uint64_t) * 2 * kFrames;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        UINT64 frequency = 0;
        if (FAILED(m_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                     D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                     IID_PPV_ARGS(&m_queryReadback))) ||
            FAILED(m_queue->GetTimestampFrequency(&frequency)) || frequency == 0) {
            m_queryHeap.Reset();
            m_queryReadback.Reset();
        } else {
            m_timestampToMs = 1000.0 / double(frequency);
        }
    }
    m_fenceEvent = ok ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    ok = ok && m_fenceEvent;

    d3d12::RendererDesc desc;
    desc.device = m_device.Get();
    desc.framesInFlight = kFrames;  // our slots and the renderer's buffer sets advance together, one per Render
    desc.rtvFormat = scDesc.BufferDesc.Format;
    ok = ok && m_renderer.Init(desc);
    if (!ok) {
        Shutdown();
        return false;
    }
    m_frameCounter = 0;
    if (m_onDevice) m_onDevice(m_renderer);
    return true;
}

bool D3D12Overlay::Render(IDXGISwapChain* swapChain, const DrawData& data) {
    if (!swapChain) return false;
    if (data.TotalPrimCount() == 0) {
        // Nothing to draw (the menu is closed): no command list, no barriers, no submission, no fence.
        if (m_device) m_renderer.SkipFrame();
        m_stats.renderCpuMs = 0.0f;
        m_stats.prims = m_stats.drawCalls = 0;
        return true;
    }
    if (!EnsureSetup(swapChain)) return false;

    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapChain3;
    DXGI_SWAP_CHAIN_DESC scDesc;
    if (FAILED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3))) || FAILED(swapChain->GetDesc(&scDesc))) {
        return false;
    }
    const Vec2 displaySize{float(scDesc.BufferDesc.Width), float(scDesc.BufferDesc.Height)};
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f) return false;
    if (scDesc.BufferDesc.Format != m_formatSeen) {
        m_formatSeen = scDesc.BufferDesc.Format;
        m_encoding = DetectColorEncoding(swapChain, m_formatSeen, m_colorSpace);
    }

    // Our allocators ring in lockstep with the renderer's own frame slots (both advance once per call); waiting on
    // this slot's fence before reusing it satisfies the renderer's frame contract too.
    const uint32_t slotIndex = m_frameCounter % kFrames;
    FrameSlot& slot = m_frames[slotIndex];
    if (!WaitForFence(slot.fenceValue, kFenceTimeoutMs)) return false;  // our earlier work is stuck: skip a frame
    if (slot.timed && m_queryReadback) {
        const D3D12_RANGE range = {slotIndex * 2 * sizeof(uint64_t), (slotIndex * 2 + 2) * sizeof(uint64_t)};
        void* mapped = nullptr;
        if (SUCCEEDED(m_queryReadback->Map(0, &range, &mapped))) {
            const auto* ticks = reinterpret_cast<const uint64_t*>(static_cast<const uint8_t*>(mapped) + range.Begin);
            if (ticks[1] >= ticks[0]) {
                m_stats.gpuMs = float(double(ticks[1] - ticks[0]) * m_timestampToMs);
                m_stats.gpuValid = true;
            }
            const D3D12_RANGE noWrite = {0, 0};
            m_queryReadback->Unmap(0, &noWrite);
        }
        slot.timed = false;
    }
    const auto start = std::chrono::steady_clock::now();
    if (FAILED(slot.allocator->Reset()) || FAILED(m_list->Reset(slot.allocator.Get(), nullptr))) return false;

    // This frame's back buffer, released again before returning: holding back buffers between frames would make the
    // game's own ResizeBuffers fail.
    Microsoft::WRL::ComPtr<ID3D12Resource> backBuffer;
    if (FAILED(swapChain3->GetBuffer(swapChain3->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backBuffer)))) {
        m_list->Close();
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, rtv);

    // The back buffer is in PRESENT at Present time; move it to RENDER_TARGET, draw, and move it back.
    const D3D12_RESOURCE_BARRIER toRender =
        Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_list->ResourceBarrier(1, &toRender);
    if (m_queryHeap) m_list->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2);

    DrawData frame = data;
    frame.displaySize = displaySize;
    d3d12::RenderParams params;
    params.colorEncoding = m_encoding;
    params.paperWhiteNits = m_paperWhiteNits;
    params.rtv = rtv;
    params.rtvFormat = scDesc.BufferDesc.Format;
    m_renderer.Render(m_list.Get(), frame, params);

    if (m_queryHeap) {
        m_list->EndQuery(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2 + 1);
        m_list->ResolveQueryData(m_queryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slotIndex * 2, 2,
                                 m_queryReadback.Get(), uint64_t(slotIndex) * 2 * sizeof(uint64_t));
    }
    const D3D12_RESOURCE_BARRIER toPresent =
        Transition(backBuffer.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    m_list->ResourceBarrier(1, &toPresent);
    if (FAILED(m_list->Close())) return false;

    ID3D12CommandList* lists[] = {m_list.Get()};
    m_queue->ExecuteCommandLists(1, lists);
    slot.fenceValue = ++m_fenceValue;
    m_queue->Signal(m_fence.Get(), slot.fenceValue);
    slot.timed = m_queryHeap != nullptr;
    ++m_frameCounter;

    const RenderStats& rs = m_renderer.LastFrameStats();
    m_stats.prims = rs.prims;
    m_stats.drawCalls = rs.drawCalls;
    m_stats.encoding = m_encoding;
    m_stats.format = m_formatSeen;
    m_stats.renderCpuMs = MsSince(start);
    return true;
}

} // namespace drizzy::overlay
