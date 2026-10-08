#include "drizzy/overlay_d3d11.h"

#include <dxgi.h>

#include <chrono>

namespace drizzy::overlay {
namespace {

float MsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
}

} // namespace

D3D11Overlay::~D3D11Overlay() { Shutdown(); }

void D3D11Overlay::Shutdown() {
    for (Timing& t : m_timings) t = {};
    m_timingIndex = 0;
    m_renderer.Shutdown();
    m_context.Reset();
    m_device.Reset();
    m_swapChain = nullptr;
    m_formatSeen = DXGI_FORMAT_UNKNOWN;
    m_stats = {};
}

void D3D11Overlay::SetColorSpace(DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (colorSpace == m_colorSpace) return;
    m_colorSpace = colorSpace;
    m_formatSeen = DXGI_FORMAT_UNKNOWN;  // detect the encoding again
}

bool D3D11Overlay::EnsureDevice(IDXGISwapChain* swapChain) {
    ComPtr<ID3D11Device> device;
    if (!swapChain || FAILED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) return false;
    if (m_removedDevice && swapChain != m_removedSwapChain) m_removedDevice = nullptr;  // the game recreated it
    if (device.Get() == m_removedDevice) return false;
    if (m_device && device == m_device) {
        if (m_device->GetDeviceRemovedReason() == S_OK) {
            m_swapChain = swapChain;
            return true;
        }
        // The device is gone (driver reset, GPU removed): stop until the game brings up a new one.
        m_removedDevice = m_device.Get();
        m_removedSwapChain = swapChain;
        Shutdown();
        return false;
    }
    // First use, or the game moved to another device: everything is rebuilt on the new one.
    Shutdown();
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    if (!context || !m_renderer.Init({device.Get(), context.Get()})) return false;
    m_device = device;
    m_context = context;
    m_swapChain = swapChain;
    for (Timing& t : m_timings) {
        D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
        m_device->CreateQuery(&desc, &t.disjoint);
        desc.Query = D3D11_QUERY_TIMESTAMP;
        m_device->CreateQuery(&desc, &t.begin);
        m_device->CreateQuery(&desc, &t.end);
    }
    if (m_onDevice) m_onDevice(m_renderer);
    return true;
}

void D3D11Overlay::ReadTimings() {
    // Never waits: results that are not ready yet are picked up on a later frame.
    for (uint32_t i = 1; i <= kTimings; ++i) {
        Timing& t = m_timings[(m_timingIndex + i) % kTimings];
        if (!t.pending || !t.disjoint) continue;
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        if (m_context->GetData(t.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) {
            continue;
        }
        t.pending = false;
        UINT64 begin = 0, end = 0;
        if (!disjoint.Disjoint && disjoint.Frequency &&
            m_context->GetData(t.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            m_context->GetData(t.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
            end >= begin) {
            m_stats.gpuMs = float(double(end - begin) * 1000.0 / double(disjoint.Frequency));
            m_stats.gpuValid = true;
        }
    }
}

bool D3D11Overlay::Render(IDXGISwapChain* swapChain, const DrawData& data) {
    if (!swapChain) return false;
    if (data.TotalPrimCount() == 0) {
        // Nothing to draw (the menu is closed): no swap chain queries, no state save/restore, no GPU work.
        if (m_device && m_swapChain == swapChain) m_renderer.SkipFrame();
        m_stats.renderCpuMs = 0.0f;
        m_stats.prims = m_stats.drawCalls = 0;
        return true;
    }
    const auto start = std::chrono::steady_clock::now();
    if (!EnsureDevice(swapChain)) return false;

    DXGI_SWAP_CHAIN_DESC scDesc;
    if (FAILED(swapChain->GetDesc(&scDesc))) return false;
    const Vec2 displaySize{float(scDesc.BufferDesc.Width), float(scDesc.BufferDesc.Height)};
    if (displaySize.x <= 0.0f || displaySize.y <= 0.0f) return false;
    if (scDesc.BufferDesc.Format != m_formatSeen) {
        m_formatSeen = scDesc.BufferDesc.Format;
        m_encoding = DetectColorEncoding(swapChain, m_formatSeen, m_colorSpace);
    }

    // A view of this frame's back buffer, released again before returning: holding back buffer references between
    // frames would make the game's own ResizeBuffers fail.
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) ||
        FAILED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv))) {
        return false;
    }

    // Save the game's bound render targets, draw over the back buffer, then restore them so the game is unaffected.
    // (The renderer's preserveState restores the rest of the pipeline.)
    ID3D11RenderTargetView* savedRtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* savedDsv = nullptr;
    m_context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, &savedDsv);
    m_context->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);

    Timing& timing = m_timings[m_timingIndex];
    const bool timed = timing.disjoint && !timing.pending;
    if (timed) {
        m_context->Begin(timing.disjoint.Get());
        m_context->End(timing.begin.Get());
    }
    DrawData frame = data;
    frame.displaySize = displaySize;  // authoritative size comes from the swap chain
    d3d11::RenderParams params;
    params.colorEncoding = m_encoding;
    params.paperWhiteNits = m_paperWhiteNits;
    params.preserveState = true;
    m_renderer.Render(frame, params);
    if (timed) {
        m_context->End(timing.end.Get());
        m_context->End(timing.disjoint.Get());
        timing.pending = true;
        m_timingIndex = (m_timingIndex + 1) % kTimings;
    }

    m_context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRtvs, savedDsv);
    for (ID3D11RenderTargetView* saved : savedRtvs) {
        if (saved) saved->Release();
    }
    if (savedDsv) savedDsv->Release();

    ReadTimings();
    const RenderStats& rs = m_renderer.LastFrameStats();
    m_stats.prims = rs.prims;
    m_stats.drawCalls = rs.drawCalls;
    m_stats.encoding = m_encoding;
    m_stats.format = m_formatSeen;
    m_stats.renderCpuMs = MsSince(start);
    return true;
}

} // namespace drizzy::overlay
