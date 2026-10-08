#include "host.h"

#include "drizzy/backend_d3d11.h"
#include "png_writer.h"

#include <d3d11.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdio>
#include <iterator>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace drizzy;

namespace sandbox {
namespace {

// GPU time between Begin and End via timestamp queries, with a few frames in flight.
class GpuTimer {
public:
    bool Init(ID3D11Device* device) {
        for (Frame& f : m_frames) {
            D3D11_QUERY_DESC desc = {D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            if (FAILED(device->CreateQuery(&desc, &f.disjoint))) return false;
            desc.Query = D3D11_QUERY_TIMESTAMP;
            if (FAILED(device->CreateQuery(&desc, &f.begin)) || FAILED(device->CreateQuery(&desc, &f.end))) return false;
        }
        return true;
    }
    void Begin(ID3D11DeviceContext* ctx) {
        if (m_written - m_read == kFrames) {
            double ignored;
            Read(ctx, ignored, true);  // ring is full: consume the oldest result before its queries are reused
        }
        Frame& f = m_frames[m_written % kFrames];
        ctx->Begin(f.disjoint.Get());
        ctx->End(f.begin.Get());
    }
    void End(ID3D11DeviceContext* ctx) {
        Frame& f = m_frames[m_written % kFrames];
        ctx->End(f.end.Get());
        ctx->End(f.disjoint.Get());
        ++m_written;
    }
    bool Poll(ID3D11DeviceContext* ctx, double& ms) { return Read(ctx, ms, false); }
    bool Wait(ID3D11DeviceContext* ctx, double& ms) {
        if (m_written == 0) return false;
        m_read = m_written - 1;
        return Read(ctx, ms, true);
    }

private:
    bool Read(ID3D11DeviceContext* ctx, double& ms, bool block) {
        if (m_read == m_written) return false;
        Frame& f = m_frames[m_read % kFrames];
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
        HRESULT hr;
        while ((hr = ctx->GetData(f.disjoint.Get(), &disjoint, sizeof(disjoint),
                                  block ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH)) == S_FALSE) {
            if (!block) return false;
            YieldProcessor();
        }
        ++m_read;
        UINT64 t0 = 0, t1 = 0;
        if (FAILED(hr) || disjoint.Disjoint || ctx->GetData(f.begin.Get(), &t0, sizeof(t0), 0) != S_OK ||
            ctx->GetData(f.end.Get(), &t1, sizeof(t1), 0) != S_OK) {
            return false;
        }
        ms = double(t1 - t0) * 1000.0 / double(disjoint.Frequency);
        return true;
    }

    static constexpr uint32_t kFrames = 4;
    struct Frame {
        ComPtr<ID3D11Query> disjoint, begin, end;
    };
    Frame m_frames[kFrames];
    uint64_t m_written = 0;
    uint64_t m_read = 0;
};

class D3D11Host final : public GpuHost {
public:
    ~D3D11Host() override {
        m_renderer.Shutdown();
        if (m_context) m_context->ClearState();
    }

    const char* ApiName() const override { return "D3D11"; }

    bool Init(HWND hwnd, uint32_t width, uint32_t height, bool warp) override {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        const D3D_DRIVER_TYPE type = warp ? D3D_DRIVER_TYPE_WARP : D3D_DRIVER_TYPE_HARDWARE;
        UINT flags = 0;
#if defined(_DEBUG)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
        HRESULT hr = D3D11CreateDevice(nullptr, type, nullptr, flags, levels, UINT(std::size(levels)),
                                       D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
        if (FAILED(hr) && (flags & D3D11_CREATE_DEVICE_DEBUG)) {
            // The debug layer is an optional Windows feature; run without it rather than fail.
            hr = D3D11CreateDevice(nullptr, type, nullptr, 0, levels, UINT(std::size(levels)), D3D11_SDK_VERSION,
                                   &m_device, nullptr, &m_context);
        }
        if (FAILED(hr)) return false;
        m_width = width;
        m_height = height;

        if (hwnd) {
            ComPtr<IDXGIDevice> dxgiDevice;
            ComPtr<IDXGIAdapter> adapter;
            ComPtr<IDXGIFactory2> factory;
            if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
                FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) {
                return false;
            }
            ComPtr<IDXGIFactory5> factory5;
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory.As(&factory5))) {
                factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
            }
            m_tearing = tearing != FALSE;
            DXGI_SWAP_CHAIN_DESC1 scd = {};
            scd.Width = width;
            scd.Height = height;
            scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd.SampleDesc.Count = 1;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.BufferCount = 2;
            scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            scd.Flags = m_tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
            if (FAILED(factory->CreateSwapChainForHwnd(m_device.Get(), hwnd, &scd, nullptr, nullptr, &m_swapChain))) {
                return false;
            }
            factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
            if (!CreateBackBufferView()) return false;
        } else {
            D3D11_TEXTURE2D_DESC td = {};
            td.Width = width;
            td.Height = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET;
            if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_offscreen)) ||
                FAILED(m_device->CreateRenderTargetView(m_offscreen.Get(), nullptr, &m_rtv))) {
                return false;
            }
        }
        return m_renderer.Init({m_device.Get(), m_context.Get()}) && m_timer.Init(m_device.Get());
    }

    bool Resize(uint32_t width, uint32_t height) override {
        if (!m_swapChain) return false;
        m_context->OMSetRenderTargets(0, nullptr, nullptr);
        m_rtv.Reset();
        DXGI_SWAP_CHAIN_DESC1 desc;
        m_swapChain->GetDesc1(&desc);
        if (FAILED(m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, desc.Flags))) return false;
        m_width = width;
        m_height = height;
        return CreateBackBufferView();
    }

    TextureId CreateTexture(uint32_t width, uint32_t height, const void* rgba) override {
        return m_renderer.CreateTexture(width, height, rgba);
    }
    void DestroyTexture(TextureId texture) override { m_renderer.DestroyTexture(texture); }

    double RenderFrame(const DrawData& data, bool vsync) override {
        m_context->OMSetRenderTargets(1, m_rtv.GetAddressOf(), nullptr);
        m_context->ClearRenderTargetView(m_rtv.Get(), kClearColor);
        m_timer.Begin(m_context.Get());
        const int64_t t0 = Now();
        m_renderer.Render(data);
        const int64_t t1 = Now();
        for (uint32_t i = 1; i < m_gpuRepeat; ++i) m_renderer.Render(data);
        m_timer.End(m_context.Get());
        if (m_swapChain) m_swapChain->Present(vsync ? 1 : 0, (!vsync && m_tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0);
        return TicksToMs(t1 - t0);
    }

    bool SetGpuRepeat(uint32_t count) override {
        m_gpuRepeat = count ? count : 1;
        return true;
    }
    bool PollGpuTime(double& ms) override { return m_timer.Poll(m_context.Get(), ms); }
    bool WaitGpuTime(double& ms) override { return m_timer.Wait(m_context.Get(), ms); }

    bool SavePng(const char* path) override {
        if (!m_offscreen) return false;
        D3D11_TEXTURE2D_DESC td;
        m_offscreen->GetDesc(&td);
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        if (FAILED(m_device->CreateTexture2D(&td, nullptr, &staging))) return false;
        m_context->CopyResource(staging.Get(), m_offscreen.Get());
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (FAILED(m_context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const bool ok = WriteOpaquePng(path, m_width, m_height, static_cast<const uint8_t*>(mapped.pData),
                                       mapped.RowPitch);
        m_context->Unmap(staging.Get(), 0);
        return ok;
    }

    RenderStats Stats() const override { return m_renderer.LastFrameStats(); }
    PrimAllocator* Allocator() override { return &m_renderer; }

    std::wstring AdapterName() const override {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> adapter;
        DXGI_ADAPTER_DESC desc = {};
        if (FAILED(m_device.As(&dxgiDevice)) || FAILED(dxgiDevice->GetAdapter(&adapter)) ||
            FAILED(adapter->GetDesc(&desc))) {
            return L"unknown adapter";
        }
        return desc.Description;
    }

    int ReportDebugMessages() override {
        ComPtr<ID3D11InfoQueue> queue;
        if (FAILED(m_device.As(&queue))) return -1;
        int problems = 0;
        const UINT64 count = queue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            queue->GetMessage(i, nullptr, &length);
            std::vector<char> storage(length);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (FAILED(queue->GetMessage(i, message, &length))) continue;
            if (message->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D11: %.*s\n", int(message->DescriptionByteLength), message->pDescription);
                ++problems;
            }
        }
        queue->ClearStoredMessages();
        return problems;
    }

private:
    bool CreateBackBufferView() {
        ComPtr<ID3D11Texture2D> backBuffer;
        return SUCCEEDED(m_swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer))) &&
               SUCCEEDED(m_device->CreateRenderTargetView(backBuffer.Get(), nullptr, &m_rtv));
    }

    ComPtr<ID3D11Device> m_device;
    ComPtr<ID3D11DeviceContext> m_context;
    ComPtr<IDXGISwapChain1> m_swapChain;
    ComPtr<ID3D11Texture2D> m_offscreen;
    ComPtr<ID3D11RenderTargetView> m_rtv;
    bool m_tearing = false;
    uint32_t m_width = 0, m_height = 0;
    d3d11::Renderer m_renderer;
    GpuTimer m_timer;
    uint32_t m_gpuRepeat = 1;
};

} // namespace

std::unique_ptr<GpuHost> CreateD3D11Host() { return std::make_unique<D3D11Host>(); }

} // namespace sandbox
