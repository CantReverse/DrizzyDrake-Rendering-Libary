// drizzy_renderer sandbox: a standalone host for developing, screenshotting and benchmarking the library on Direct3D 11
// and Direct3D 12. This is the only code in the repository that touches the OS (window, input, clipboard, cursor);
// the library never does - this file shows what a host has to feed it.
//
//   drizzy_sandbox [--api d3d11|d3d12]          interactive window; every key is rebindable in the keybinds window (F1)
//       1 shapes  2 text  3 shape stress  4 text stress  5 UI  6 gallery  7 3D debug  8 styles  9 themes
//       Mouse 4/5: previous/next scene   Up/Down: stress count   V: vsync   Ctrl+L: log frame stats   F1: keybinds
//       Esc: quit
//   drizzy_sandbox --screenshot out.png         render offscreen and save
//       --scene shapes|text|stress|textstress|ui|gallery|debug3d|styles|themes  --size WxH  --time T  --count N
//       --mouse X,Y  --click X,Y  --theme NAME  --look NAME  --font N  --background NAME  --warp
//   drizzy_sandbox --bench                      headless CPU / GPU cost for shapes, text and UI
//       --count N  --frames N  --warp  --size WxH  --gpu-repeat N  --video-memory | --system-memory  --copy-prims

#include "host.h"
#include "scenes.h"
#include "ui_demo.h"
#include "demo_gallery.h"
#include "keybinds.h"

#include <windowsx.h>

#include "drizzy/font.h"
#include "drizzy/ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

using namespace drizzy;
using namespace sandbox;

namespace {

enum class Scene { Shapes, Text, Stress, TextStress, Ui, Gallery, Debug3D, Styles, Themes };
constexpr const char* kSceneNames[] = {"shapes", "text", "stress", "textstress", "ui", "gallery", "debug3d", "styles",
                                       "themes"};
constexpr const char* kSceneLabels[] = {"Shapes",  "Text",           "Shape stress",     "Text stress",
                                        "Game UI", "Widget gallery", "3D debug drawing", "Every style",
                                        "Every theme"};
constexpr int kSceneCount = 9;

bool IsUiScene(Scene scene) {
    return scene == Scene::Ui || scene == Scene::Gallery || scene == Scene::Styles || scene == Scene::Themes;
}
bool IsStressScene(Scene scene) { return scene == Scene::Stress || scene == Scene::TextStress; }

struct Options {
    enum class Mode { Window, Screenshot, Bench } mode = Mode::Window;
    std::string api = "d3d11";
    std::string output;
    Scene scene = Scene::Shapes;
    uint32_t width = 1280, height = 720;
    uint32_t frames = 0;  // window: quit after N frames (0 = run until closed); bench: frames measured per case
    uint32_t count = 0;   // stress item count (0 = default)
    float time = 0.0f;
    Vec2 mouse = {-1e30f, -1e30f};  // screenshot: simulated mouse position (UI hover states)
    std::vector<Vec2> clicks;       // screenshot: simulated left clicks, in order
    bool warp = false;
    int theme = -1, look = -1, font = -1, background = -1;  // gallery settings (-1: default)
    PrimMemoryChoice primMemory = PrimMemoryChoice::Auto;  // D3D12: d3d12::PrimMemory
    bool copyPrims = false;  // record draw lists in their own memory (copied at render) instead of into GPU memory
    uint32_t gpuRepeat = 1;    // bench: draw each frame this many times (steadier GPU timings for small workloads)
};

void PrintUsage() {
    std::printf(
        "usage: drizzy_sandbox [options]\n"
        "  (no mode)                 interactive window\n"
        "  --screenshot <file.png>   render offscreen and save the image\n"
        "  --bench                   headless benchmark of shapes, text and UI\n"
        "  --api d3d11|d3d12         graphics API (default d3d11)\n"
        "  --scene <name>            shapes | text | stress | textstress | ui\n"
        "  --size WxH                render size (default 1280x720)\n"
        "  --count N                 stress item count\n"
        "  --frames N                frames to measure (bench) or run before quitting (window)\n"
        "  --time T                  scene time in seconds for screenshots\n"
        "  --mouse X,Y               mouse position for UI screenshots\n"
        "  --click X,Y               click there before a UI screenshot (repeatable)\n"
        "  --theme NAME              UI theme: dark light obsidian cyberpunk nord emerald crimson dracula sakura\n"
        "  --look NAME               UI style: classic soft neon flat retro glass\n"
        "  --font N                  gallery font index (0 = Inter)\n"
        "  --background NAME         gallery backdrop: none constellation matrix starfield synthwave waves bokeh\n"
        "                            snow gradient\n"
        "  --warp                    use the WARP software rasterizer\n"
        "  --video-memory            D3D12: per-frame prims in video memory (GPU upload heap; default if supported)\n"
        "  --system-memory           D3D12: per-frame prims in system memory (upload heap)\n"
        "  --copy-prims              record draw lists in their own memory and copy them at render, instead of\n"
        "                            straight into GPU memory\n"
        "  --gpu-repeat N            bench, D3D11: draw each frame N times and report GPU time per draw\n");
}

// Index of `name` among `count` names produced by `nameOf` (case-insensitive, prefix allowed), or -1.
template <typename NameOf>
int FindByName(const char* name, int count, NameOf nameOf) {
    const size_t length = std::strlen(name);
    for (int i = 0; i < count; ++i) {
        if (_strnicmp(nameOf(i), name, length) == 0) return i;
    }
    return -1;
}

bool ParseOptions(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const char* value = (i + 1 < argc) ? argv[i + 1] : nullptr;
        auto takeValue = [&]() {
            ++i;
            return value != nullptr;
        };
        if (arg == "--screenshot") {
            if (!takeValue()) return false;
            o.mode = Options::Mode::Screenshot;
            o.output = value;
        } else if (arg == "--bench") {
            o.mode = Options::Mode::Bench;
        } else if (arg == "--api") {
            if (!takeValue()) return false;
            o.api = value;
            if (o.api != "d3d11" && o.api != "d3d12") return false;
        } else if (arg == "--scene") {
            if (!takeValue()) return false;
            const std::string name = value;
            bool found = false;
            for (int s = 0; s < kSceneCount; ++s) {
                if (name == kSceneNames[s]) {
                    o.scene = Scene(s);
                    found = true;
                }
            }
            if (!found) return false;
        } else if (arg == "--size") {
            if (!takeValue() || sscanf_s(value, "%ux%u", &o.width, &o.height) != 2 || !o.width || !o.height) return false;
        } else if (arg == "--mouse") {
            if (!takeValue() || sscanf_s(value, "%f,%f", &o.mouse.x, &o.mouse.y) != 2) return false;
        } else if (arg == "--click") {
            Vec2 p;
            if (!takeValue() || sscanf_s(value, "%f,%f", &p.x, &p.y) != 2) return false;
            o.clicks.push_back(p);
        } else if (arg == "--count") {
            if (!takeValue()) return false;
            o.count = uint32_t(std::strtoul(value, nullptr, 10));
        } else if (arg == "--frames") {
            if (!takeValue()) return false;
            o.frames = uint32_t(std::strtoul(value, nullptr, 10));
        } else if (arg == "--time") {
            if (!takeValue()) return false;
            o.time = std::strtof(value, nullptr);
        } else if (arg == "--theme") {
            if (!takeValue()) return false;
            o.theme = FindByName(value, int(UiTheme::Count), [](int t) { return UiStyle::ThemeName(UiTheme(t)); });
            if (o.theme < 0) return false;
        } else if (arg == "--look") {
            if (!takeValue()) return false;
            o.look = FindByName(value, int(UiLook::Count), [](int l) { return UiStyle::LookName(UiLook(l)); });
            if (o.look < 0) return false;
        } else if (arg == "--font") {
            if (!takeValue()) return false;
            o.font = int(std::strtol(value, nullptr, 10));
        } else if (arg == "--background") {
            if (!takeValue()) return false;
            o.background = FindByName(value, int(demo::BackdropKind::Count),
                                      [](int b) { return demo::BackdropName(demo::BackdropKind(b)); });
            if (o.background < 0) return false;
        } else if (arg == "--warp") {
            o.warp = true;
        } else if (arg == "--video-memory") {
            o.primMemory = PrimMemoryChoice::Video;
        } else if (arg == "--system-memory") {
            o.primMemory = PrimMemoryChoice::System;
        } else if (arg == "--copy-prims") {
            o.copyPrims = true;
        } else if (arg == "--gpu-repeat") {
            if (!takeValue()) return false;
            o.gpuRepeat = std::max(1u, uint32_t(std::strtoul(value, nullptr, 10)));
        } else {
            return false;
        }
    }
    return true;
}

std::unique_ptr<GpuHost> CreateHost(const Options& o) {
    return o.api == "d3d12" ? CreateD3D12Host(o.primMemory) : CreateD3D11Host();
}

// Debug builds: prints what the API's debug layer reported and turns any warning into a failing exit code.
int ReportDebugLayer(GpuHost& host) {
    const int problems = host.ReportDebugMessages();
    if (problems >= 0) std::printf("%s debug layer: %d warning(s)/error(s)\n", host.ApiName(), problems);
    return problems > 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Clipboard for the UI, provided by the host through UiPlatform.
// ---------------------------------------------------------------------------------------------------------------------
const char* GetClipboard(void*) {
    static std::string text;
    text.clear();
    if (!OpenClipboard(nullptr)) return nullptr;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(handle))) {
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (bytes > 0) {
                text.resize(size_t(bytes));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
                text.resize(size_t(bytes - 1));
            }
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return text.c_str();
}

void SetClipboard(void*, const char* utf8) {
    const int chars = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (chars <= 0) return;
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, size_t(chars) * sizeof(wchar_t));
    if (!memory) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, static_cast<wchar_t*>(GlobalLock(memory)), chars);
    GlobalUnlock(memory);
    if (OpenClipboard(nullptr)) {
        EmptyClipboard();
        if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
        CloseClipboard();
    } else {
        GlobalFree(memory);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Everything the scenes need
// ---------------------------------------------------------------------------------------------------------------------
struct App {
    FontAtlas atlas;
    SceneAssets assets;
    double atlasBuildMs = 0.0;
    StressScene stress;
    TextStressScene textStress;
    uint32_t stressCount = 20000;
    uint32_t textStressCount = 5000;
    bool stressDirty = true;
    Ui ui;
    UiDemo demo;
    DemoStats stats;
    demo::KeyBinds binds;      // set up by SetupKeybinds
    bool showKeybinds = false;  // the keybinds window, shown over any scene
    DrawList dl;
    const DrawList* singleList[1] = {&dl};
    DrawData singleData;
    std::vector<const DrawList*> overlayLists;  // a rendering scene with the keybinds window over it
    DrawData overlayData;

    bool Load(GpuHost& host, bool recordIntoGpuMemory) {
        const int64_t t0 = Now();
        Font* font = atlas.AddFontDefault();
        if (!font) return false;
        demo::Gallery().fonts = demo::AddDemoFonts(atlas, font);  // the gallery's font menu
        if (!atlas.Build()) return false;
        atlasBuildMs = TicksToMs(Now() - t0);
        atlas.SetTexture(host.CreateTexture(atlas.Width(), atlas.Height(), atlas.Pixels()));
        atlas.ReleasePixels();
        const std::vector<uint32_t> checker = MakeCheckerPixels(64);
        assets.font = font;
        assets.checker = host.CreateTexture(64, 64, checker.data());
        ui.Style().font = font;
        ui.Platform().getClipboardText = GetClipboard;
        ui.Platform().setClipboardText = SetClipboard;
        if (recordIntoGpuMemory) {
            // Every list is rebuilt each frame and rendered right after, so they can all write straight into the
            // renderer's GPU memory.
            ui.SetPrimAllocator(host.Allocator());
            dl.SetPrimAllocator(host.Allocator());
        }
        stats.api = host.ApiName();
        return atlas.Texture() != kNoTexture && assets.checker != kNoTexture;
    }
    void Release(GpuHost& host) {
        host.DestroyTexture(assets.checker);
        host.DestroyTexture(atlas.Texture());
    }

    // Builds one frame of `scene` and returns what to render.
    const DrawData& Build(Scene scene, Vec2 size, float time, float deltaTime) {
        const bool uiScene = IsUiScene(scene);
        const bool keybindsWindow = showKeybinds;
        if (uiScene || keybindsWindow) {
            ui.Input().displaySize = size;
            ui.Input().deltaTime = deltaTime;
            ui.NewFrame();
        }
        if (uiScene) {
            if (scene == Scene::Gallery) {
                demo::ShowWidgetGallery(ui, assets.checker);
            } else if (scene == Scene::Styles || scene == Scene::Themes) {
                demo::ShowStyleShowcase(ui, scene == Scene::Themes);
            } else {
                demo.Build(ui, time, stats, assets.checker);
            }
            if (keybindsWindow) binds.ShowWindow(ui, &showKeybinds);
            return ui.Render();
        }
        if (!keybindsWindow) ui.Input().characters.clear();  // typing in other scenes must not pile up for the UI
        dl.Reset(Rect({0.0f, 0.0f}, size));
        switch (scene) {
        case Scene::Shapes: BuildShapesScene(dl, time, assets); break;
        case Scene::Text: BuildTextScene(dl, time, assets); break;
        case Scene::Debug3D: BuildDebug3DScene(dl, time, assets, size); break;
        case Scene::Stress:
            if (stressDirty || stress.Count() != stressCount) stress.Generate(stressCount, size);
            stressDirty = false;
            stress.Build(dl, time);
            break;
        default:
            if (stressDirty || textStress.Count() != textStressCount) textStress.Generate(textStressCount, size);
            stressDirty = false;
            textStress.Build(dl, *assets.font, time);
            break;
        }
        if (keybindsWindow) {
            // The scene's list first, then the UI's lists over it.
            binds.ShowWindow(ui, &showKeybinds);
            const DrawData& uiData = ui.Render();
            overlayLists.assign(1, &dl);
            overlayLists.insert(overlayLists.end(), uiData.lists, uiData.lists + uiData.listCount);
            overlayData.lists = overlayLists.data();
            overlayData.listCount = uint32_t(overlayLists.size());
            overlayData.displaySize = size;
            return overlayData;
        }
        singleData.lists = singleList;
        singleData.listCount = 1;
        singleData.displaySize = size;
        return singleData;
    }
};

void SetupKeybinds(App& app);

// --theme, --look, --font and --background: the gallery's settings, applied to the UI's style.
void ApplyGalleryOptions(const Options& o, Ui& ui) {
    demo::GallerySettings& s = demo::Gallery();
    if (o.theme >= 0) s.theme = UiTheme(o.theme);
    if (o.look >= 0) s.look = UiLook(o.look);
    if (o.font >= 0) s.font = o.font;
    if (o.background >= 0) s.backdrop.kind = demo::BackdropKind(o.background);
    demo::ApplyGallerySettings(ui);
}

// =====================================================================================================================
// Headless modes
// =====================================================================================================================
int RunScreenshot(const Options& o) {
    std::unique_ptr<GpuHost> host = CreateHost(o);
    auto app = std::make_unique<App>();
    if (!host->Init(nullptr, o.width, o.height, o.warp) || !app->Load(*host, !o.copyPrims)) {
        std::fprintf(stderr, "%s initialization failed\n", host->ApiName());
        return 1;
    }
    const Vec2 size{float(o.width), float(o.height)};
    if (o.count) app->stressCount = app->textStressCount = o.count;
    ApplyGalleryOptions(o, app->ui);
    SetupKeybinds(*app);  // default bindings only, so a --click on the main menu's Keybinds button can show them
    // UI windows size themselves to their content over the first frames: let them settle, replay the clicks (a press
    // frame, a release frame, and a frame for any popup to measure itself), then settle again.
    UiInput& in = app->ui.Input();
    auto frame = [&] { host->RenderFrame(app->Build(o.scene, size, o.time, 1.0f / 60.0f), false); };
    const int settle = IsUiScene(o.scene) ? 3 : 1;
    for (int i = 0; i < settle; ++i) frame();
    for (const Vec2& click : o.clicks) {
        in.mousePos = click;
        in.mouseDown[0] = true;
        frame();
        in.mouseDown[0] = false;
        frame();
        frame();
    }
    in.mousePos = o.mouse;
    for (int i = 0; i < settle; ++i) frame();
    const RenderStats stats = host->Stats();
    const bool saved = host->SavePng(o.output.c_str());
    app->Release(*host);
    if (!saved) {
        std::fprintf(stderr, "failed to write %s\n", o.output.c_str());
        return 1;
    }
    std::printf("wrote %s (%s, %ux%u, %s scene, %u prims, %u draw calls, %ls)\n", o.output.c_str(), host->ApiName(),
                o.width, o.height, kSceneNames[int(o.scene)], stats.prims, stats.drawCalls,
                host->AdapterName().c_str());
    return ReportDebugLayer(*host);
}

int RunBench(const Options& o) {
    std::unique_ptr<GpuHost> host = CreateHost(o);
    auto app = std::make_unique<App>();
    const bool sizeGiven = o.width != 1280 || o.height != 720;  // the window default; the bench defaults to 1080p
    const uint32_t width = sizeGiven ? o.width : 1920, height = sizeGiven ? o.height : 1080;
    if (!host->Init(nullptr, width, height, o.warp) || !app->Load(*host, !o.copyPrims)) {
        std::fprintf(stderr, "%s initialization failed\n", host->ApiName());
        return 1;
    }
    const Vec2 size{float(width), float(height)};

    std::printf("drizzy_renderer benchmark | %s | %ls | %ux%u%s\n", host->ApiName(), host->AdapterName().c_str(), width,
                height,
#if defined(_DEBUG)
                " | DEBUG BUILD (numbers are not representative)"
#else
                ""
#endif
    );
    if (*host->Details()) std::printf("%s\n", host->Details());
    std::printf("%s\n", o.copyPrims ? "draw lists record into their own memory; Render copies them (--copy-prims)"
                                    : "draw lists record straight into GPU memory; Render copies nothing");
    std::printf("font atlas: %ux%u built in %.1f ms (all glyphs + kerning, multithreaded)\n\n", app->atlas.Width(),
                app->atlas.Height(), app->atlasBuildMs);
    std::printf("  build  = CPU time to record the frame (draw list, or the whole UI frame)\n");
    std::printf("  submit = CPU time inside the backend's Render call (upload + %s)\n",
                o.api == "d3d12" ? "command recording" : "draw calls");
    const uint32_t gpuRepeat = o.gpuRepeat > 1 && host->SetGpuRepeat(o.gpuRepeat) ? o.gpuRepeat : 1u;
    if (gpuRepeat > 1) {
        std::printf("  gpu    = GPU time to draw (timestamp queries; each frame drawn %u times, time per draw)\n",
                    gpuRepeat);
    } else {
        std::printf("  gpu    = GPU time to draw (timestamp queries)\n");
    }

    // Runs one case; `units` is what the last column divides the build time by (0 = prims).
    auto runCase = [&](uint32_t count, uint32_t units, const std::function<const DrawData&(float)>& build) {
        const uint32_t frames = o.frames ? o.frames : (count >= 100000 ? 30u : 200u);
        const uint32_t warmup = 5;
        double buildMs = 0.0, submitMs = 0.0, gpuMs = 0.0;
        uint32_t gpuSamples = 0;
        for (uint32_t frame = 0; frame < warmup + frames; ++frame) {
            const int64_t t0 = Now();
            const DrawData& data = build(float(frame) * 0.016f);
            const double recordMs = TicksToMs(Now() - t0);
            const double renderMs = host->RenderFrame(data, false);
            double gpu = 0.0;
            const bool gotGpu = host->WaitGpuTime(gpu);
            if (frame >= warmup) {
                buildMs += recordMs;
                submitMs += renderMs;
                if (gotGpu) {
                    gpuMs += gpu;
                    ++gpuSamples;
                }
            }
        }
        const RenderStats stats = host->Stats();
        buildMs /= frames;
        std::printf("%10u %10u %6u %10.3f %10.3f %10.3f %12.1f\n", count, stats.prims, stats.drawCalls, buildMs,
                    submitMs / frames, gpuSamples ? gpuMs / gpuSamples / gpuRepeat : 0.0,
                    buildMs * 1e6 / double(units ? units : std::max(stats.prims, 1u)));
    };
    const char* header = "%10s %10s %6s %10s %10s %10s %12s\n";
    DrawList& dl = app->dl;
    auto single = [&]() -> const DrawData& {
        app->singleData.lists = app->singleList;
        app->singleData.listCount = 1;
        app->singleData.displaySize = size;
        return app->singleData;
    };

    std::vector<uint32_t> shapeCounts = {1000, 10000, 100000};
    std::vector<uint32_t> labelCounts = {100, 1000, 10000};
    std::vector<uint32_t> rowCounts = {100, 1000, 10000};
    if (!o.warp) {
        shapeCounts.push_back(1000000);
        labelCounts.push_back(100000);
    }
    if (o.count) shapeCounts = labelCounts = rowCounts = {o.count};

    std::printf("\nShapes: rounded rects, circles, outlined rects, lines, plain rects (6-28 px), all moving\n");
    std::printf(header, "shapes", "prims", "draws", "build ms", "submit ms", "gpu ms", "ns/prim");
    for (uint32_t count : shapeCounts) {
        app->stress.Generate(count, size);
        runCase(count, 0, [&](float t) -> const DrawData& {
            dl.Reset(Rect({0.0f, 0.0f}, size));
            app->stress.Build(dl, t);
            return single();
        });
    }

    std::printf("\nText: labels of 2-4 words (11-20 px), all moving; prims = glyphs\n");
    std::printf(header, "labels", "prims", "draws", "build ms", "submit ms", "gpu ms", "ns/glyph");
    for (uint32_t count : labelCounts) {
        app->textStress.Generate(count, size);
        runCase(count, 0, [&](float t) -> const DrawData& {
            dl.Reset(Rect({0.0f, 0.0f}, size));
            app->textStress.Build(dl, *app->assets.font, t);
            return single();
        });
    }

    std::printf("\nUI: rows of text + button + checkbox + slider in one scrolling window (visible rows are drawn,\n"
                "the rest are laid out and clipped); build = the whole UI frame, NewFrame to Render\n");
    std::printf(header, "rows", "prims", "draws", "build ms", "submit ms", "gpu ms", "ns/widget");
    for (uint32_t rows : rowCounts) {
        std::vector<float> values(rows, 0.5f);
        std::vector<char> checks(rows, 0);
        Ui& ui = app->ui;
        runCase(rows, rows * 4, [&](float) -> const DrawData& {
            ui.Input().displaySize = size;
            ui.NewFrame();
            ui.SetNextWindowPos({20.0f, 20.0f});
            ui.SetNextWindowSize(ui.GetDisplaySize() - Vec2(40.0f, 40.0f));
            if (ui.Begin("Benchmark")) {
                for (uint32_t i = 0; i < rows; ++i) {
                    ui.PushID(int(i));
                    ui.AlignTextToFramePadding();
                    ui.TextF("Row %u", i);
                    ui.SameLine(120.0f);
                    ui.Button("Use");
                    ui.SameLine();
                    bool checked = checks[i] != 0;
                    if (ui.Checkbox("##on", &checked)) checks[i] = checked ? 1 : 0;
                    ui.SameLine();
                    ui.SetNextItemWidth(300.0f);
                    ui.SliderFloat("##value", &values[i], 0.0f, 1.0f);
                    ui.PopID();
                }
            }
            ui.End();
            return ui.Render();
        });
    }

    std::printf("\nUI with ListClipper: the same rows, but only the visible ones are submitted\n");
    std::printf(header, "rows", "prims", "draws", "build ms", "submit ms", "gpu ms", "ns/row");
    for (uint32_t rows : rowCounts) {
        std::vector<float> values(rows, 0.5f);
        std::vector<char> checks(rows, 0);
        Ui& ui = app->ui;
        runCase(rows, rows, [&](float) -> const DrawData& {
            ui.Input().displaySize = size;
            ui.NewFrame();
            ui.SetNextWindowPos({20.0f, 20.0f});
            ui.SetNextWindowSize(ui.GetDisplaySize() - Vec2(40.0f, 40.0f));
            if (ui.Begin("Benchmark")) {
                ListClipper clipper;
                clipper.Begin(ui, int(rows));
                while (clipper.Step()) {
                    for (int i = clipper.displayStart; i < clipper.displayEnd; ++i) {
                        ui.PushID(i);
                        ui.AlignTextToFramePadding();
                        ui.TextF("Row %d", i);
                        ui.SameLine(120.0f);
                        ui.Button("Use");
                        ui.SameLine();
                        bool checked = checks[size_t(i)] != 0;
                        if (ui.Checkbox("##on", &checked)) checks[size_t(i)] = checked ? 1 : 0;
                        ui.SameLine();
                        ui.SetNextItemWidth(300.0f);
                        ui.SliderFloat("##value", &values[size_t(i)], 0.0f, 1.0f);
                        ui.PopID();
                    }
                }
            }
            ui.End();
            return ui.Render();
        });
    }

    std::printf("\nWindows: N stacked near-full-screen windows with a few widgets each (GPU fill cost of large\n"
                "windows: shadow, background, border, title bar)\n");
    std::printf(header, "windows", "prims", "draws", "build ms", "submit ms", "gpu ms", "ns/window");
    for (uint32_t windows : {1u, 4u, 16u}) {
        Ui& ui = app->ui;
        runCase(windows, windows, [&](float) -> const DrawData& {
            ui.Input().displaySize = size;
            ui.NewFrame();
            for (uint32_t i = 0; i < windows; ++i) {
                char name[32];
                std::snprintf(name, sizeof(name), "Fill %u", i);
                ui.SetNextWindowPos({20.0f + 8.0f * float(i), 20.0f + 8.0f * float(i)});
                ui.SetNextWindowSize(ui.GetDisplaySize() - Vec2(40.0f + 8.0f * float(windows), 40.0f + 8.0f * float(windows)));
                if (ui.Begin(name)) {
                    ui.Text("A large window");
                    ui.Button("Button");
                }
                ui.End();
            }
            return ui.Render();
        });
    }
    app->Release(*host);
    return ReportDebugLayer(*host);
}

// =====================================================================================================================
// Interactive window
// =====================================================================================================================
struct WindowState {
    uint32_t width = 0, height = 0;
    bool resized = false;
    bool minimized = false;
    Scene scene = Scene::Shapes;
    bool vsync = false;
    int countChange = 0;  // +1 / -1 requested by the stress count keybinds
    bool quitRequested = false;
    bool mouseTracked = false;
    wchar_t highSurrogate = 0;
    Ui* ui = nullptr;
    bool uiVisible = false;  // the last frame ran the UI (a UI scene, or the keybinds window)
    demo::KeyBinds* binds = nullptr;

    // A text field has keyboard focus: keybinds must not run.
    bool Typing() const { return uiVisible && ui && ui->Output().wantCaptureKeyboard; }
} g_window;

bool MapKey(WPARAM vk, Key& key) {
    switch (vk) {
    case VK_TAB: key = Key::Tab; return true;
    case VK_LEFT: key = Key::LeftArrow; return true;
    case VK_RIGHT: key = Key::RightArrow; return true;
    case VK_UP: key = Key::UpArrow; return true;
    case VK_DOWN: key = Key::DownArrow; return true;
    case VK_PRIOR: key = Key::PageUp; return true;
    case VK_NEXT: key = Key::PageDown; return true;
    case VK_HOME: key = Key::Home; return true;
    case VK_END: key = Key::End; return true;
    case VK_INSERT: key = Key::Insert; return true;
    case VK_DELETE: key = Key::Delete; return true;
    case VK_BACK: key = Key::Backspace; return true;
    case VK_SPACE: key = Key::Space; return true;
    case VK_RETURN: key = Key::Enter; return true;
    case VK_ESCAPE: key = Key::Escape; return true;
    case 'A': key = Key::A; return true;
    case 'C': key = Key::C; return true;
    case 'V': key = Key::V; return true;
    case 'X': key = Key::X; return true;
    case 'Y': key = Key::Y; return true;
    case 'Z': key = Key::Z; return true;
    default: return false;
    }
}

LPCWSTR CursorResource(MouseCursor cursor) {
    switch (cursor) {
    case MouseCursor::TextInput: return IDC_IBEAM;
    case MouseCursor::Hand: return IDC_HAND;
    case MouseCursor::ResizeAll: return IDC_SIZEALL;
    case MouseCursor::ResizeNS: return IDC_SIZENS;
    case MouseCursor::ResizeEW: return IDC_SIZEWE;
    case MouseCursor::ResizeNWSE: return IDC_SIZENWSE;
    case MouseCursor::ResizeNESW: return IDC_SIZENESW;
    default: return IDC_ARROW;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    UiInput* in = g_window.ui ? &g_window.ui->Input() : nullptr;
    switch (msg) {
    case WM_SIZE:
        g_window.minimized = wParam == SIZE_MINIMIZED;
        if (!g_window.minimized) {
            g_window.width = LOWORD(lParam);
            g_window.height = HIWORD(lParam);
            g_window.resized = true;
        }
        return 0;
    case WM_MOUSEMOVE:
        if (in) in->mousePos = {float(GET_X_LPARAM(lParam)), float(GET_Y_LPARAM(lParam))};
        if (!g_window.mouseTracked) {
            TRACKMOUSEEVENT track = {sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            g_window.mouseTracked = true;
        }
        return 0;
    case WM_MOUSELEAVE:
        if (in) in->mousePos = {-1e30f, -1e30f};
        g_window.mouseTracked = false;
        return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: {
        const int button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) ? 0
                         : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK) ? 1 : 2;
        if (in) in->mouseDown[button] = true;
        SetCapture(hwnd);  // keep receiving the release when dragging outside the window
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP: case WM_MBUTTONUP: {
        const int button = msg == WM_LBUTTONUP ? 0 : msg == WM_RBUTTONUP ? 1 : 2;
        if (in) in->mouseDown[button] = false;
        if (in && !in->mouseDown[0] && !in->mouseDown[1] && !in->mouseDown[2]) ReleaseCapture();
        return 0;
    }
    case WM_MOUSEWHEEL:
        if (in) in->mouseWheel += float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
        return 0;
    case WM_MOUSEHWHEEL:
        if (in) in->mouseWheelH += float(GET_WHEEL_DELTA_WPARAM(wParam)) / float(WHEEL_DELTA);
        return 0;
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
        // The extra mouse buttons (Mouse 4 / 5) are for keybinds only; the UI uses left, right and middle.
        if (g_window.binds && g_window.binds->OnMessage(msg, wParam, lParam, g_window.Typing())) return TRUE;
        break;
    case WM_CHAR:
        if (in) {
            const wchar_t c = wchar_t(wParam);
            if (c >= 0xD800 && c < 0xDC00) {
                g_window.highSurrogate = c;
            } else if (c >= 0xDC00 && c < 0xE000 && g_window.highSurrogate) {
                in->AddCharacter(0x10000u + ((uint32_t(g_window.highSurrogate) - 0xD800u) << 10) + (uint32_t(c) - 0xDC00u));
                g_window.highSurrogate = 0;
            } else {
                in->AddCharacter(c);
            }
        }
        return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP: {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        // Keybinds see presses first: a bound key runs its command (unless a text field has focus), and while a
        // binding is being set the press becomes the new binding. Either way the UI and the window never see it.
        if (down && g_window.binds && g_window.binds->OnMessage(msg, wParam, lParam, g_window.Typing())) return 0;
        Key key;
        if (in && MapKey(wParam, key)) in->SetKey(key, down);
        if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP) break;  // keep Alt+F4 working
        return 0;
    }
    case WM_SYSCOMMAND:
        // Alt alone, or Alt + an unbound letter, would open the window menu (and beep: there is none). Alt
        // combinations are keybinds here.
        if ((wParam & 0xFFF0) == SC_KEYMENU) return 0;
        break;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT && g_window.uiVisible && g_window.ui) {
            SetCursor(LoadCursorW(nullptr, CursorResource(g_window.ui->Output().cursor)));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// The sandbox's commands: the shortcuts it always had, now rebindable, plus a few debug commands.
void SetupKeybinds(App& app) {
    demo::KeyBinds& binds = app.binds;
    app.demo.keybindsWindow = &app.showKeybinds;

    binds.Group("Scenes");
    for (int s = 0; s < kSceneCount; ++s) {
        binds.Add(std::string("scene.") + kSceneNames[s], kSceneLabels[s], {uint8_t('1' + s)},
                  [s] { g_window.scene = Scene(s); });
    }
    binds.Add("scene.previous", "Previous scene", {VK_XBUTTON1},
              [] { g_window.scene = Scene((int(g_window.scene) + kSceneCount - 1) % kSceneCount); });
    binds.Add("scene.next", "Next scene", {VK_XBUTTON2},
              [] { g_window.scene = Scene((int(g_window.scene) + 1) % kSceneCount); });

    binds.Group("Stress scenes");
    const auto inStress = [] { return IsStressScene(g_window.scene); };
    binds.Add("stress.more", "Double the item count", {VK_UP}, [] { g_window.countChange = 1; }, inStress);
    binds.Add("stress.fewer", "Halve the item count", {VK_DOWN}, [] { g_window.countChange = -1; }, inStress);

    binds.Group("Sandbox");
    binds.Add("vsync", "Toggle vsync", {'V'}, [] { g_window.vsync = !g_window.vsync; });
    binds.Add("keybinds", "Keybinds window", {VK_F1}, [&app] { app.showKeybinds = !app.showKeybinds; });
    binds.Add("log_stats", "Log frame stats", {'L', true}, [&app] {  // to the console
        const DemoStats& s = app.stats;
        std::printf("[%s | %s] %.0f fps | build %.3f ms | submit %.3f ms | gpu %.3f ms | %u prims | %u draw calls\n",
                    s.api, kSceneNames[int(g_window.scene)], double(s.fps), s.buildMs, s.submitMs, s.gpuMs, s.prims,
                    s.drawCalls);
        std::fflush(stdout);
    });
    // Not in the UI scenes, where Esc closes popups.
    binds.Add("quit", "Quit", {VK_ESCAPE}, [] { g_window.quitRequested = true; },
              [] { return !IsUiScene(g_window.scene); });
}

// The bindings persist in a text file next to the executable.
std::filesystem::path KeybindsPath() {
    wchar_t exe[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(exe).replace_filename(L"drizzy_sandbox_keybinds.ini");
}

void LoadKeybinds(demo::KeyBinds& binds) {
    const std::filesystem::path path = KeybindsPath();
    if (path.empty()) return;
    std::ifstream file(path);
    if (file) binds.Load(std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()));
}

void SaveKeybinds(const demo::KeyBinds& binds) {
    const std::filesystem::path path = KeybindsPath();
    if (path.empty()) return;
    std::ofstream file(path, std::ios::trunc);
    if (file) file << binds.Save();
}

int RunWindowed(const Options& o) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"DrizzySandbox";
    RegisterClassExW(&wc);
    RECT rc = {0, 0, LONG(o.width), LONG(o.height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"drizzy_renderer sandbox", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top, nullptr, nullptr, wc.hInstance,
                                nullptr);
    if (!hwnd) return 1;
    GetClientRect(hwnd, &rc);
    g_window.width = uint32_t(rc.right - rc.left);
    g_window.height = uint32_t(rc.bottom - rc.top);
    g_window.scene = o.scene;

    std::unique_ptr<GpuHost> host = CreateHost(o);
    auto app = std::make_unique<App>();
    if (!host->Init(hwnd, g_window.width, g_window.height, o.warp) || !app->Load(*host, !o.copyPrims)) {
        std::fprintf(stderr, "%s initialization failed\n", host->ApiName());
        return 1;
    }
    if (o.count) app->stressCount = app->textStressCount = o.count;
    ApplyGalleryOptions(o, app->ui);
    g_window.ui = &app->ui;
    SetupKeybinds(*app);
    LoadKeybinds(app->binds);
    g_window.binds = &app->binds;
    const std::wstring adapterName = host->AdapterName();

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    g_window.resized = false;

    const int64_t start = Now();
    int64_t statsStart = start, lastFrame = start;
    double buildSum = 0.0, submitSum = 0.0, gpuSum = 0.0;
    uint32_t statFrames = 0, gpuFrames = 0, frameIndex = 0;
    bool running = true;
    while (running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!running) break;
        if (g_window.minimized) {
            Sleep(10);
            continue;
        }
        if (g_window.resized && g_window.width > 0 && g_window.height > 0) {
            g_window.resized = false;
            if (!host->Resize(g_window.width, g_window.height)) break;
            app->stressDirty = true;
        }
        if (g_window.countChange) {
            uint32_t& count = g_window.scene == Scene::TextStress ? app->textStressCount : app->stressCount;
            count = g_window.countChange > 0 ? std::min(count * 2, 1u << 22) : std::max(count / 2, 100u);
            g_window.countChange = 0;
        }
        UiInput& in = app->ui.Input();
        in.keyCtrl = GetKeyState(VK_CONTROL) < 0;
        in.keyShift = GetKeyState(VK_SHIFT) < 0;
        in.keyAlt = GetKeyState(VK_MENU) < 0;

        const Vec2 size{float(g_window.width), float(g_window.height)};
        const int64_t now = Now();
        const float deltaTime = float(TicksToMs(now - lastFrame) / 1000.0);
        lastFrame = now;
        const float time = float(TicksToMs(now - start) / 1000.0);
        g_window.uiVisible = IsUiScene(g_window.scene) || app->showKeybinds;
        const int64_t t0 = Now();
        const DrawData& data = app->Build(g_window.scene, size, time, deltaTime);
        const double buildMs = TicksToMs(Now() - t0);
        const double submitMs = host->RenderFrame(data, g_window.vsync);
        buildSum += buildMs;
        submitSum += submitMs;
        ++statFrames;
        double gpuMs;
        while (host->PollGpuTime(gpuMs)) {
            gpuSum += gpuMs;
            ++gpuFrames;
            app->stats.gpuMs = gpuMs;
        }
        const RenderStats stats = host->Stats();
        app->stats.PushFrameTime(deltaTime * 1000.0f);
        app->stats.buildMs = buildMs;
        app->stats.submitMs = submitMs;
        app->stats.prims = stats.prims;
        app->stats.drawCalls = stats.drawCalls;
        if (app->binds.TakeChanged()) SaveKeybinds(app->binds);
        if (app->demo.quitRequested || g_window.quitRequested) DestroyWindow(hwnd);

        const double elapsed = TicksToMs(Now() - statsStart);
        if (elapsed >= 500.0) {
            app->stats.fps = float(statFrames * 1000.0 / elapsed);
            const demo::KeyCombo* keybindsKey = app->binds.Find("keybinds");
            const std::string keybindsHint = keybindsKey ? demo::KeyComboName(*keybindsKey) : "Unbound";
            char title[512];
            std::snprintf(title, sizeof(title),
                          "drizzy_renderer sandbox | %s | %s | %.0f fps | build %.3f ms | submit %.3f ms | gpu %.3f ms "
                          "| %u prims | %u draws | vsync %s | keybinds: %s | %ls",
                          host->ApiName(), kSceneNames[int(g_window.scene)], double(app->stats.fps),
                          buildSum / statFrames, submitSum / statFrames, gpuFrames ? gpuSum / gpuFrames : 0.0,
                          stats.prims, stats.drawCalls, g_window.vsync ? "on" : "off", keybindsHint.c_str(),
                          adapterName.c_str());
            SetWindowTextA(hwnd, title);
            statsStart = Now();
            buildSum = submitSum = gpuSum = 0.0;
            statFrames = gpuFrames = 0;
        }
        if (o.frames && ++frameIndex >= o.frames) DestroyWindow(hwnd);
    }
    g_window.ui = nullptr;
    g_window.binds = nullptr;
    app->Release(*host);
    return ReportDebugLayer(*host);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, options)) {
        PrintUsage();
        return 2;
    }
    switch (options.mode) {
    case Options::Mode::Screenshot: return RunScreenshot(options);
    case Options::Mode::Bench: return RunBench(options);
    default: return RunWindowed(options);
    }
}
