// drizzy_renderer injectable overlay DLL.
//
// A ready-to-inject overlay: injected into one of your own games, it draws the drizzy widget gallery over the game's
// frame from a hooked IDXGISwapChain::Present, working on both D3D11 and D3D12 games with one build. The drizzy
// library installs no hooks; this sample does, which is the split the project is designed around - the DLL is the
// loader, the library is the renderer.
//
// It hooks by overwriting vtable entries (see vmt_hook.h): the DXGI swap chain's Present and ResizeBuffers (shared by
// D3D11 and D3D12, so one hook covers both), and - for D3D12, where a Present hook cannot see the command queue the
// game submits with - ID3D12CommandQueue::ExecuteCommandLists, fed to drizzy's D3D12QueueCapture. The swap chain and
// command-queue vtables are read from throwaway objects created at startup; every object the game makes shares them.
//
// Insert toggles the menu; End unloads the DLL. This is a developer/mod tool for your own games: it does not read
// other processes, target other players, or try to hide from anything.

#include "vmt_hook.h"

#include "drizzy/font.h"
#include "drizzy/ui.h"
#include "drizzy/win32_input.h"
#include "drizzy/overlay_d3d11.h"
#include "drizzy/overlay_d3d12.h"

#include "demo_gallery.h"

#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace drizzy;

namespace {

// Vtable slots. IUnknown is 0-2; the rest follow the interface inheritance chain.
constexpr unsigned kSwapChainPresent = 8;         // IDXGISwapChain::Present
constexpr unsigned kSwapChainResizeBuffers = 13;  // IDXGISwapChain::ResizeBuffers
constexpr unsigned kQueueExecuteCommandLists = 10;  // ID3D12CommandQueue::ExecuteCommandLists

enum class Api { Unknown, D3D11, D3D12 };

// Everything the hooks share. One instance for the life of the DLL; built on the setup thread, used on the render
// and window threads.
struct Overlay {
    std::atomic<bool> ready{false};   // hooks installed, UI built
    std::atomic<bool> unloading{false};
    bool menuVisible = true;

    Api api = Api::Unknown;
    HWND hwnd = nullptr;
    WNDPROC originalWndProc = nullptr;

    Ui ui;
    FontAtlas atlas;
    Win32Input input;
    overlay::D3D11Overlay d11;
    overlay::D3D12Overlay d12;
    overlay::D3D12QueueCapture queueCapture;
    TextureId checker = kNoTexture;
    std::chrono::steady_clock::time_point lastFrame;  // for the real per-frame delta time
    bool haveLastFrame = false;

    vmt::Hook presentHook, resizeHook, executeHook;
    HANDLE setupThread = nullptr;
    HANDLE unloadEvent = nullptr;
    HMODULE self = nullptr;
};

Overlay* g = nullptr;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecuteFn = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// A 64x64 checker for the gallery's Image / ImageButton demo.
std::vector<uint32_t> MakeChecker() {
    std::vector<uint32_t> pixels(64 * 64);
    for (uint32_t y = 0; y < 64; ++y) {
        for (uint32_t x = 0; x < 64; ++x) {
            const uint32_t r = x * 4, gc = y * 4;
            pixels[y * 64 + x] = ((x / 8) + (y / 8)) & 1 ? Rgba(r, gc, 230) : Rgba(r / 3, gc / 3, 70);
        }
    }
    return pixels;
}

void BuildMenu(Ui& ui) {
    ui.SetNextWindowPos({ui.GetDisplaySize().x - 300.0f, 40.0f}, Cond::FirstUseEver);
    ui.SetNextWindowSize({260.0f, 0.0f}, Cond::FirstUseEver);
    if (ui.Begin("drizzy overlay", nullptr, WindowFlags::AlwaysAutoResize)) {
        const char* api = g->api == Api::D3D12 ? "D3D12" : "D3D11";
        const overlay::OverlayStats& s = g->api == Api::D3D12 ? g->d12.LastFrameStats() : g->d11.LastFrameStats();
        ui.TextF("%s  |  injected overlay", api);
        ui.TextF("overlay cpu %.3f ms", double(s.renderCpuMs));
        if (s.gpuValid) ui.TextF("overlay gpu %.3f ms", double(s.gpuMs));
        ui.TextF("%u prims, %u draw calls", s.prims, s.drawCalls);
        ui.TextDisabled("Insert: toggle   End: unload");
    }
    ui.End();
    demo::ShowWidgetGallery(ui, g->checker);
}

// ---------------------------------------------------------------------------------------------------------------------
// Hooked WndProc: the game's window procedure, with the overlay given first look at input.
// ---------------------------------------------------------------------------------------------------------------------
LRESULT CALLBACK HookedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g && g->ready.load(std::memory_order_acquire)) {
        if (msg == WM_KEYDOWN && wParam == VK_INSERT) {
            g->menuVisible = !g->menuVisible;
            return 0;
        }
        if (msg == WM_KEYDOWN && wParam == VK_END) {
            SetEvent(g->unloadEvent);  // the setup thread tears everything down
            return 0;
        }
        if (g->menuVisible) {
            if (g->input.ProcessMessage(g->ui, hwnd, msg, wParam, lParam)) return true;
            if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
                if (HCURSOR c = Win32Input::Cursor(g->ui)) {
                    SetCursor(c);
                    return TRUE;
                }
            }
        }
    }
    return CallWindowProcW(g->originalWndProc, hwnd, msg, wParam, lParam);
}

// On the first Present: identify the API, hook the game's window, and upload the font atlas once the overlay has a
// device. Returns false if the overlay could not be set up this frame (drawing is skipped, the game is untouched).
bool LazyInit(IDXGISwapChain* swapChain) {
    if (g->api == Api::Unknown) {
        ComPtr<ID3D12Device> d12;
        g->api = SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&d12))) ? Api::D3D12 : Api::D3D11;
        ComPtr<ID3D12Device> device;
        if (g->api == Api::D3D12 && SUCCEEDED(swapChain->GetDevice(IID_PPV_ARGS(&device)))) {
            g->queueCapture.SetDevice(device.Get());  // only this game's present queue
        }
        // Whenever the overlay (re)creates a device, its textures must be made on that device: the font atlas and the
        // gallery's checker. The atlas keeps its pixels (not released) so it can be re-uploaded after a device change.
        auto uploadAssets = [](auto& renderer) {
            g->atlas.SetTexture(renderer.CreateTexture(g->atlas.Width(), g->atlas.Height(), g->atlas.Pixels()));
            const std::vector<uint32_t> pixels = MakeChecker();
            g->checker = renderer.CreateTexture(64, 64, pixels.data());
            g->ui.SetPrimAllocator(&renderer);  // record straight into the overlay's GPU memory
        };
        g->d11.SetDeviceCallback(uploadAssets);
        g->d12.SetDeviceCallback(uploadAssets);
    }
    if (!g->hwnd) {
        DXGI_SWAP_CHAIN_DESC desc;
        if (SUCCEEDED(swapChain->GetDesc(&desc)) && desc.OutputWindow) {
            g->hwnd = desc.OutputWindow;
            g->originalWndProc =
                reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(HookedWndProc)));
        }
    }
    return g->hwnd != nullptr;
}

void RenderOverlay(IDXGISwapChain* swapChain) {
    if (g->unloading.load(std::memory_order_acquire) || !g->ready.load(std::memory_order_acquire)) return;
    if (!LazyInit(swapChain)) return;

    // Real frame time: the UI advances its animations and timers by this, so they run at wall-clock speed whatever the
    // game's frame rate is. Without it the UI would use UiInput's default 1/60 s per frame and run fast on a high-refresh
    // game (e.g. 4x on a 240 Hz display).
    const auto now = std::chrono::steady_clock::now();
    float dt = 1.0f / 60.0f;
    if (g->haveLastFrame) dt = std::chrono::duration<float>(now - g->lastFrame).count();
    g->lastFrame = now;
    g->haveLastFrame = true;

    g->input.NewFrame(g->ui, g->hwnd);
    g->ui.Input().deltaTime = Clamp(dt, 1.0f / 1000.0f, 0.1f);  // guard against stalls and a bad first delta
    g->ui.NewFrame();
    if (g->menuVisible) BuildMenu(g->ui);
    const DrawData& data = g->ui.Render();

    if (g->api == Api::D3D12) {
        if (ID3D12CommandQueue* queue = g->queueCapture.PresentQueue()) g->d12.SetCommandQueue(queue);
        g->d12.Render(swapChain, data);
        g->queueCapture.OnPresent();
    } else {
        g->d11.Render(swapChain, data);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------------------------------------------------
HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    if (!(flags & DXGI_PRESENT_TEST)) RenderOverlay(swapChain);  // TEST presents draw nothing; skip them
    return g->presentHook.Original<PresentFn>()(swapChain, syncInterval, flags);
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
                                              DXGI_FORMAT format, UINT flags) {
    if (g->ready.load(std::memory_order_acquire) && !g->unloading.load(std::memory_order_acquire)) {
        if (g->api == Api::D3D12) g->d12.OnResizeBuffers();
        else g->d11.OnResizeBuffers();
    }
    return g->resizeHook.Original<ResizeBuffersFn>()(swapChain, bufferCount, width, height, format, flags);
}

void STDMETHODCALLTYPE HookedExecuteCommandLists(ID3D12CommandQueue* queue, UINT numLists,
                                                 ID3D12CommandList* const* lists) {
    if (g->ready.load(std::memory_order_acquire)) g->queueCapture.OnExecuteCommandLists(queue);
    g->executeHook.Original<ExecuteFn>()(queue, numLists, lists);
}

// ---------------------------------------------------------------------------------------------------------------------
// Startup: read the vtables from throwaway objects, install the hooks, build the UI.
// ---------------------------------------------------------------------------------------------------------------------
HWND MakeDummyWindow() {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"drizzy_overlay_dummy";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 16, 16, nullptr, nullptr,
                           wc.hInstance, nullptr);
}

// A throwaway D3D11 device + swap chain, to read the IDXGISwapChain vtable (shared with D3D12 swap chains).
bool HookSwapChainVTable(HWND dummy) {
    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 1;
    scd.BufferDesc.Width = 16;
    scd.BufferDesc.Height = 16;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = dummy;
    scd.SampleDesc.Count = 1;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
                                             D3D11_SDK_VERSION, &scd, &swapChain, &device, nullptr, &context))) {
        return false;
    }
    g->presentHook.Install(swapChain.Get(), kSwapChainPresent, reinterpret_cast<void*>(&HookedPresent));
    g->resizeHook.Install(swapChain.Get(), kSwapChainResizeBuffers, reinterpret_cast<void*>(&HookedResizeBuffers));
    return g->presentHook.Installed();
}

// A throwaway D3D12 device + command queue, to read the ID3D12CommandQueue vtable. Best-effort: a D3D11-only game
// does not need it, and the hook simply never fires.
void HookCommandQueueVTable() {
    ComPtr<ID3D12Device> device;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return;
    D3D12_COMMAND_QUEUE_DESC desc = {};
    desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&queue)))) return;
    g->executeHook.Install(queue.Get(), kQueueExecuteCommandLists, reinterpret_cast<void*>(&HookedExecuteCommandLists));
}

DWORD WINAPI SetupThread(LPVOID) {
    HWND dummy = MakeDummyWindow();
    const bool hooked = dummy && HookSwapChainVTable(dummy);
    if (hooked) HookCommandQueueVTable();
    if (dummy) DestroyWindow(dummy);
    if (!hooked) {
        g->unloading.store(true, std::memory_order_release);
        FreeLibraryAndExitThread(g->self, 1);
    }

    // The font atlas is CPU-only here; its texture is uploaded in the first Present, once a device exists.
    Font* font = g->atlas.AddFontDefault();
    g->atlas.Build();
    g->ui.Style().font = font;
    g->ui.Platform().getClipboardText = &Win32GetClipboard;
    g->ui.Platform().setClipboardText = &Win32SetClipboard;
    g->ready.store(true, std::memory_order_release);

    WaitForSingleObject(g->unloadEvent, INFINITE);

    // Unload: stop drawing, restore the vtable entries and the window procedure, let any in-flight hooked call return,
    // then release GPU resources and leave.
    g->unloading.store(true, std::memory_order_release);
    g->ready.store(false, std::memory_order_release);
    g->presentHook.Remove();
    g->resizeHook.Remove();
    g->executeHook.Remove();
    if (g->hwnd && g->originalWndProc) {
        SetWindowLongPtrW(g->hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g->originalWndProc));
    }
    Sleep(200);  // drain presents still inside a hook body before destroying what they touch
    g->d11.Shutdown();
    g->d12.Shutdown();
    FreeLibraryAndExitThread(g->self, 0);  // does not return
}

} // namespace

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
        g = new Overlay();
        g->self = instance;
        g->unloadEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g->setupThread = CreateThread(nullptr, 0, SetupThread, nullptr, 0, nullptr);
        if (g->setupThread) CloseHandle(g->setupThread);
    }
    // Teardown happens on the setup thread via FreeLibraryAndExitThread, not here: DLL_PROCESS_DETACH during
    // FreeLibrary would run while that thread is still unwinding.
    return TRUE;
}
