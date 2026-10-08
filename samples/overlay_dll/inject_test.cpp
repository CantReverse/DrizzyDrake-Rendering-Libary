// Test host for the injectable overlay DLL: a stand-in "game" that renders a plain frame and presents in a loop, with
// no overlay code of its own. It loads drizzy_overlay_dll.dll itself (standing in for an external injector), so the
// DLL's swap-chain Present hook draws the widget gallery over the frames. --api picks a D3D11 or D3D12 "game" so both
// of the DLL's hook paths can be exercised (D3D12 also needs the ExecuteCommandLists hook to find the present queue).
// With --screenshot it captures the composited window; otherwise it opens an interactive window (Insert toggles the
// menu, End unloads the DLL).
//
//   drizzy_overlay_dll_test [--api d3d11|d3d12] [--screenshot out.png] [--frames N]
//
// In a real game you would inject the DLL (CreateRemoteThread + LoadLibrary, or any injector) rather than loading it
// from inside the process; the hook path it exercises is identical.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "png_writer.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

bool g_quit = false;

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DESTROY) {
        g_quit = true;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool PumpAndContinue() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return !g_quit;
}

// A drifting gradient clear color, so each frame differs - the "game" rendering, unaware of the overlay.
void FrameColor(uint32_t frame, float out[4]) {
    const float t = float(frame) * 0.02f;
    out[0] = 0.09f + 0.04f * std::sin(t);
    out[1] = 0.11f;
    out[2] = 0.17f + 0.05f * std::cos(t * 0.7f);
    out[3] = 1.0f;
}

// Captures the composited window - the game frame plus the overlay the DLL drew into it - with PrintWindow, which
// grabs a window's DirectX contents through DWM. (Reading the swap chain's back buffer after Present is unreliable:
// the swap chain is free to recycle it.) The window must be visible for DWM to have composited it.
bool CaptureWindow(HWND hwnd, const char* path, uint32_t width, uint32_t height) {
    constexpr UINT kCaptureFlags = 0x00000001 | 0x00000002;  // PW_CLIENTONLY | PW_RENDERFULLCONTENT
    HDC windowDC = GetDC(hwnd);
    HDC memDC = CreateCompatibleDC(windowDC);
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
    bmi.bmiHeader.biWidth = LONG(width);
    bmi.bmiHeader.biHeight = -LONG(height);  // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(windowDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBitmap = SelectObject(memDC, dib);
    const BOOL printed = PrintWindow(hwnd, memDC, kCaptureFlags);
    bool ok = false;
    if (printed && bits) {
        std::vector<uint8_t> rgba(size_t(width) * height * 4);  // GDI hands back BGRA; the PNG writer wants RGBA
        const auto* src = static_cast<const uint8_t*>(bits);
        for (size_t i = 0; i < size_t(width) * height; ++i) {
            rgba[i * 4 + 0] = src[i * 4 + 2];
            rgba[i * 4 + 1] = src[i * 4 + 1];
            rgba[i * 4 + 2] = src[i * 4 + 0];
            rgba[i * 4 + 3] = 255;
        }
        ok = sandbox::WriteOpaquePng(path, width, height, rgba.data(), width * 4);
    }
    SelectObject(memDC, oldBitmap);
    DeleteObject(dib);
    DeleteDC(memDC);
    ReleaseDC(hwnd, windowDC);
    return ok;
}

// Drives a per-frame "game" render + present until done, loading the DLL first and capturing at the end.
template <typename RenderFrame>
int RunGame(HWND hwnd, bool headless, uint32_t frames, const std::string& screenshot, RenderFrame renderFrame) {
    HMODULE dll = LoadLibraryW(L"drizzy_overlay_dll.dll");
    if (!dll) {
        std::fprintf(stderr, "could not load drizzy_overlay_dll.dll (error %lu)\n", GetLastError());
        return 1;
    }
    std::printf("loaded drizzy_overlay_dll.dll; its Present hook draws the gallery over this host's frames\n");
    Sleep(800);  // let the DLL's setup thread install the hooks and build the font atlas before we measure

    uint32_t frame = 0;
    while (PumpAndContinue()) {
        renderFrame(frame);  // renders the game frame and presents; the DLL's hook draws the overlay inside Present
        ++frame;
        if (headless && frame == frames) {
            Sleep(120);  // let DWM composite the final presented frame before capturing it
            break;
        }
        if (!headless && frames && frame >= frames) break;
    }

    // Ask the DLL to unload (its End-key handler), then give its setup thread time to restore the hooks and release
    // GPU resources before this host tears down its device.
    PostMessageW(hwnd, WM_KEYDOWN, VK_END, 0);
    Sleep(400);
    (void)screenshot;
    return 0;
}

// =====================================================================================================================
// Direct3D 11 "game"
// =====================================================================================================================
int RunD3D11(HWND hwnd, bool headless, uint32_t frames, const std::string& screenshot, uint32_t width,
             uint32_t height) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = width;
    scd.BufferDesc.Height = height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;
    UINT flags = D3D11_CREATE_DEVICE_DEBUG;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, 1,
                                               D3D11_SDK_VERSION, &scd, &swapChain, &device, nullptr, &ctx);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1, D3D11_SDK_VERSION,
                                           &scd, &swapChain, &device, nullptr, &ctx);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "D3D11 device/swap chain creation failed\n");
        return 1;
    }
    ComPtr<ID3D11Texture2D> backBuffer;
    ComPtr<ID3D11RenderTargetView> rtv;
    swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    device->CreateRenderTargetView(backBuffer.Get(), nullptr, &rtv);

    const int rc = RunGame(hwnd, headless, frames, screenshot, [&](uint32_t frame) {
        float clear[4];
        FrameColor(frame, clear);
        ctx->OMSetRenderTargets(1, rtv.GetAddressOf(), nullptr);
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        swapChain->Present(1, 0);
    });
    if (rc != 0) return rc;
    if (headless && !screenshot.empty() && CaptureWindow(hwnd, screenshot.c_str(), width, height)) {
        std::printf("wrote %s\n", screenshot.c_str());
    }

    int problems = 0;
    ComPtr<ID3D11InfoQueue> infoQueue;
    if (SUCCEEDED(device.As(&infoQueue))) {
        const UINT64 count = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            infoQueue->GetMessage(i, nullptr, &length);
            std::vector<char> storage(length);
            auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, m, &length)) && m->Severity <= D3D11_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D11: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
                ++problems;
            }
        }
    }
    std::printf("host D3D11 debug layer: %d warning(s)/error(s)\n", problems);
    return problems > 0 ? 1 : 0;
}

// =====================================================================================================================
// Direct3D 12 "game" - its own device, queue, command list and fence, so the DLL must find the present queue through
// its ExecuteCommandLists hook (the swap chain does not hand it out).
// =====================================================================================================================
int RunD3D12(HWND hwnd, bool headless, uint32_t frames, const std::string& screenshot, uint32_t width,
             uint32_t height) {
    constexpr uint32_t kBuffers = 3;
    ComPtr<ID3D12Debug> debug;  // always on in this test host, so the overlay's own D3D12 calls are validated too
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) return 1;
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
        std::fprintf(stderr, "D3D12 device creation failed\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = width;
    scd.Height = height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = kBuffers;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> swapChain1;
    if (FAILED(factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &scd, nullptr, nullptr, &swapChain1))) {
        std::fprintf(stderr, "D3D12 swap chain creation failed\n");
        return 1;
    }
    ComPtr<IDXGISwapChain3> swapChain;
    swapChain1.As(&swapChain);

    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kBuffers, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvInc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ComPtr<ID3D12Resource> buffers[kBuffers];
    for (uint32_t i = 0; i < kBuffers; ++i) {
        swapChain->GetBuffer(i, IID_PPV_ARGS(&buffers[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += size_t(i) * rtvInc;
        device->CreateRenderTargetView(buffers[i].Get(), nullptr, h);
    }
    ComPtr<ID3D12CommandAllocator> allocators[kBuffers];
    for (auto& a : allocators) device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    ComPtr<ID3D12GraphicsCommandList> list;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[0].Get(), nullptr, IID_PPV_ARGS(&list));
    list->Close();
    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    uint64_t fenceValue = 0;
    uint64_t frameFence[kBuffers] = {};

    auto barrier = [](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br = {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition.pResource = r;
        br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        br.Transition.StateBefore = a;
        br.Transition.StateAfter = b;
        return br;
    };

    const int rc = RunGame(hwnd, headless, frames, screenshot, [&](uint32_t frame) {
        const UINT index = swapChain->GetCurrentBackBufferIndex();
        if (fence->GetCompletedValue() < frameFence[index]) {
            fence->SetEventOnCompletion(frameFence[index], fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
        allocators[index]->Reset();
        list->Reset(allocators[index].Get(), nullptr);
        D3D12_RESOURCE_BARRIER toRt = barrier(buffers[index].Get(), D3D12_RESOURCE_STATE_PRESENT,
                                              D3D12_RESOURCE_STATE_RENDER_TARGET);
        list->ResourceBarrier(1, &toRt);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += size_t(index) * rtvInc;
        float clear[4];
        FrameColor(frame, clear);
        list->ClearRenderTargetView(rtv, clear, 0, nullptr);
        D3D12_RESOURCE_BARRIER toPresent = barrier(buffers[index].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &toPresent);
        list->Close();
        ID3D12CommandList* lists[] = {list.Get()};
        queue->ExecuteCommandLists(1, lists);  // the DLL's ExecuteCommandLists hook sees this and finds the queue
        swapChain->Present(1, 0);              // the DLL's Present hook draws the overlay
        frameFence[index] = ++fenceValue;
        queue->Signal(fence.Get(), fenceValue);
    });

    // Flush before capture and teardown.
    ++fenceValue;
    queue->Signal(fence.Get(), fenceValue);
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, INFINITE);
    }
    if (rc == 0 && headless && !screenshot.empty() && CaptureWindow(hwnd, screenshot.c_str(), width, height)) {
        std::printf("wrote %s\n", screenshot.c_str());
    }
    CloseHandle(fenceEvent);

    int problems = 0;
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device.As(&infoQueue))) {
        const UINT64 count = infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T length = 0;
            infoQueue->GetMessage(i, nullptr, &length);
            std::vector<char> storage(length);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(infoQueue->GetMessage(i, m, &length)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                std::fprintf(stderr, "D3D12: %.*s\n", int(m->DescriptionByteLength), m->pDescription);
                ++problems;
            }
        }
    }
    std::printf("host D3D12 debug layer: %d warning(s)/error(s)\n", problems);
    return rc != 0 ? rc : (problems > 0 ? 1 : 0);
}

} // namespace

int main(int argc, char** argv) {
    std::string api = "d3d11", screenshot;
    uint32_t frames = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--api" && i + 1 < argc) api = argv[++i];
        else if (arg == "--screenshot" && i + 1 < argc) screenshot = argv[++i];
        else if (arg == "--frames" && i + 1 < argc) frames = uint32_t(std::strtoul(argv[++i], nullptr, 10));
    }
    const bool headless = !screenshot.empty();
    if (frames == 0 && headless) frames = 60;
    const uint32_t width = 1280, height = 720;

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"drizzy_overlay_dll_test";
    RegisterClassExW(&wc);
    RECT rect = {0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"drizzy overlay DLL test (a stand-in game)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top, nullptr,
                                nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);  // visible either way: DWM must composite it for the capture

    const int rc = api == "d3d12" ? RunD3D12(hwnd, headless, frames, screenshot, width, height)
                                   : RunD3D11(hwnd, headless, frames, screenshot, width, height);
    DestroyWindow(hwnd);
    return rc;
}
