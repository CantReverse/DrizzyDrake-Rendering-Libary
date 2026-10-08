// Sandbox-only abstraction over the two backends, so every scene, screenshot and benchmark runs on either API.
// (The library itself has no such layer: a game links exactly one backend.)
#pragma once

#include <windows.h>

#include "drizzy/draw_list.h"

#include <cstdint>
#include <memory>
#include <string>

namespace sandbox {

constexpr float kClearColor[4] = {0x18 / 255.0f, 0x18 / 255.0f, 0x1F / 255.0f, 1.0f};

inline int64_t Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline double TicksToMs(int64_t ticks) {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return double(f.QuadPart);
    }();
    return double(ticks) * 1000.0 / freq;
}

class GpuHost {
public:
    virtual ~GpuHost() = default;
    virtual const char* ApiName() const = 0;
    // Renders into a swap chain for `hwnd`, or into an offscreen width x height target when hwnd is null.
    virtual bool Init(HWND hwnd, uint32_t width, uint32_t height, bool warp) = 0;
    virtual bool Resize(uint32_t width, uint32_t height) = 0;
    virtual drizzy::TextureId CreateTexture(uint32_t width, uint32_t height, const void* rgba) = 0;
    virtual void DestroyTexture(drizzy::TextureId texture) = 0;
    // Clears the target, draws `data` between GPU timestamps and presents when windowed. Returns the CPU time spent
    // inside the backend's Render call, in milliseconds.
    virtual double RenderFrame(const drizzy::DrawData& data, bool vsync) = 0;
    double RenderFrame(const drizzy::DrawList& list, drizzy::Vec2 size, bool vsync) {
        const drizzy::DrawList* lists[] = {&list};
        drizzy::DrawData data;
        data.lists = lists;
        data.listCount = 1;
        data.displaySize = size;
        return RenderFrame(data, vsync);
    }
    virtual bool PollGpuTime(double& ms) = 0;  // oldest finished measurement, never blocks
    virtual bool WaitGpuTime(double& ms) = 0;  // most recent measurement, blocks until it is ready
    virtual bool SavePng(const char* path) = 0;  // offscreen hosts only
    virtual drizzy::RenderStats Stats() const = 0;
    virtual std::wstring AdapterName() const = 0;
    virtual const char* Details() const { return ""; }  // backend-specific notes for benchmark output
    // Benchmarks: draw each frame's data this many times between the GPU timestamps, so small workloads keep the GPU
    // busy (and clocked up) long enough to measure. Returns false when the host cannot (D3D12: one Render per frame).
    virtual bool SetGpuRepeat(uint32_t) { return false; }
    // The renderer as a PrimAllocator: draw lists given it record straight into GPU memory (no copy at render time).
    virtual drizzy::PrimAllocator* Allocator() = 0;
    // Prints the debug layer's warnings and errors; returns how many, or -1 when the debug layer is not active.
    virtual int ReportDebugMessages() = 0;
};

// Where the D3D12 backend keeps per-frame prims (d3d12::PrimMemory).
enum class PrimMemoryChoice { Auto, System, Video };

std::unique_ptr<GpuHost> CreateD3D11Host();
std::unique_ptr<GpuHost> CreateD3D12Host(PrimMemoryChoice primMemory);

} // namespace sandbox
