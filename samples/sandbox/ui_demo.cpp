#include "ui_demo.h"

#include <cmath>
#include <cstdio>
#include <iterator>

using namespace drizzy;

namespace sandbox {

void UiDemo::Build(Ui& ui, float time, const DemoStats& stats, TextureId image) {
    BuildWorld(ui, time);
    BuildMainMenu(ui);
    if (m_showSettings) BuildSettings(ui);
    if (m_showDebug) BuildDebug(ui, time, stats, image);
}

// The "game": a sky, hills and drifting lights drawn behind every window, plus a HUD.
void UiDemo::BuildWorld(Ui& ui, float time) {
    DrawList& bg = ui.BackgroundDrawList();
    const Vec2 size = ui.GetDisplaySize();
    bg.AddRectFilledMultiColor(Rect({0, 0}, size), Hex(0x1A2340), Hex(0x2B1F45), Hex(0x3D2340), Hex(0x14223A));
    for (int layer = 0; layer < 3; ++layer) {
        const float base = size.y * (0.62f + 0.1f * float(layer));
        const float amp = 40.0f - 10.0f * float(layer);
        Vec2 hill[34];
        for (int i = 0; i <= 31; ++i) {
            const float x = size.x * float(i) / 31.0f;
            hill[i] = {x, base - amp * (0.6f + 0.4f * std::sin(x * 0.006f * float(layer + 1) + float(layer) * 1.7f + time * 0.05f))};
        }
        hill[32] = {size.x, size.y};
        hill[33] = {0.0f, size.y};
        bg.AddConcavePolyFilled(hill, 34, Hex(layer == 0 ? 0x2A2F5A : layer == 1 ? 0x222650 : 0x191C3C));
    }
    for (int i = 0; i < 24; ++i) {
        const float x = std::fmod(float(i) * 97.0f + time * (12.0f + float(i % 5) * 4.0f), size.x + 40.0f) - 20.0f;
        const float y = size.y * 0.15f + std::fmod(float(i) * 53.0f, size.y * 0.45f) + std::sin(time + float(i)) * 6.0f;
        bg.AddShadow(Rect::FromCenter({x, y}, {2, 2}), Hex(0xFFD27A, 160), 10.0f, 2.0f);
        bg.AddCircleFilled({x, y}, 2.5f, Hex(0xFFF1C9));
    }

    // HUD: still in the background list, so menus and tools draw over it.
    DrawList& hud = ui.BackgroundDrawList();
    const Font& font = *ui.Style().font;
    const Rect bar = Rect::FromPosSize({24.0f, size.y - 48.0f}, {260.0f, 18.0f});
    const float health = 0.72f + 0.05f * std::sin(time * 1.5f);
    hud.AddRectFilled(bar.Expanded(3.0f), Rgba(0, 0, 0, 140), 8.0f);
    hud.AddRectGradient(Rect(bar.min, {bar.min.x + bar.Width() * health, bar.max.y}), Hex(0xE8455A), Hex(0xFF8A5B),
                        Gradient::Horizontal, 6.0f);
    TextStyle hudText;
    hudText.outlineWidth = 1.5f;
    hudText.outlineColor = Rgba(0, 0, 0, 200);
    char hp[32];
    std::snprintf(hp, sizeof(hp), "HP %d / 100", int(health * 100.0f));
    hud.AddText(font, 16.0f, {bar.min.x, bar.min.y - 26.0f}, hp, hudText);
    hudText.color = Hex(0xFFE08A);
    const Vec2 ammoSize = font.MeasureText("24 / 120", 34.0f);
    hud.AddText(font, 34.0f, {size.x - ammoSize.x - 28.0f, size.y - ammoSize.y - 20.0f}, "24 / 120", hudText);
    const Vec2 c = size * 0.5f;
    for (int i = 0; i < 4; ++i) {
        const Vec2 dir = i == 0 ? Vec2(1, 0) : i == 1 ? Vec2(-1, 0) : i == 2 ? Vec2(0, 1) : Vec2(0, -1);
        hud.AddLine(c + dir * 6.0f, c + dir * 14.0f, Rgba(255, 255, 255, 220), 2.0f, LineCap::Round);
    }
}

void UiDemo::BuildMainMenu(Ui& ui) {
    const Font* font = ui.Style().font;
    ui.SetNextWindowPos({40.0f, ui.GetDisplaySize().y * 0.42f}, Cond::Always, {0.0f, 0.5f});
    ui.PushStyleVar(UiStyleVar::WindowPadding, Vec2(24.0f, 22.0f));
    ui.PushStyleVar(UiStyleVar::WindowRounding, 14.0f);
    ui.PushStyleColor(UiColor::WindowBg, Rgba(12, 14, 22, 200));
    const uint32_t flags = WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove |
                           WindowFlags::AlwaysAutoResize;
    if (ui.Begin("Main Menu", nullptr, flags)) {
        ui.PushFont(font, 40.0f);
        ui.TextColored(Hex(0xFFE08A), "DRIZZY");
        ui.PopFont();
        ui.TextDisabled("immediate-mode game UI");
        ui.Spacing();
        ui.PushFont(font, 20.0f);
        ui.PushStyleVar(UiStyleVar::FramePadding, Vec2(18.0f, 10.0f));
        ui.PushStyleVar(UiStyleVar::FrameRounding, 10.0f);
        ui.PushStyleVar(UiStyleVar::ButtonTextAlign, Vec2(0.0f, 0.5f));
        const Vec2 buttonSize(240.0f, 0.0f);
        ui.Button("Continue", buttonSize);
        if (ui.Button(m_showSettings ? "Hide settings###settings" : "Settings###settings", buttonSize)) {
            m_showSettings = !m_showSettings;
        }
        if (ui.Button(m_showDebug ? "Hide debug###debug" : "Debug tools###debug", buttonSize)) m_showDebug = !m_showDebug;
        if (keybindsWindow &&
            ui.Button(*keybindsWindow ? "Hide keybinds###keybinds" : "Keybinds###keybinds", buttonSize)) {
            *keybindsWindow = !*keybindsWindow;
        }
        ui.PushStyleColor(UiColor::Button, Hex(0x8A2B3A));
        ui.PushStyleColor(UiColor::ButtonHovered, Hex(0xB23A4E));
        ui.PushStyleColor(UiColor::ButtonActive, Hex(0xD24A60));
        if (ui.Button("Quit", buttonSize)) ui.OpenPopup("Quit to desktop?");
        ui.PopStyleColor(3);
        ui.PopStyleVar(3);
        ui.PopFont();

        if (ui.BeginPopupModal("Quit to desktop?")) {
            ui.Text("Unsaved progress will be lost.");
            ui.Spacing();
            if (ui.Button("Quit", {120.0f, 0.0f})) {
                quitRequested = true;
                ui.CloseCurrentPopup();
            }
            ui.SameLine();
            if (ui.Button("Cancel", {120.0f, 0.0f})) ui.CloseCurrentPopup();
            ui.EndPopup();
        }
    }
    ui.End();
    ui.PopStyleColor();
    ui.PopStyleVar(2);
}

void UiDemo::BuildSettings(Ui& ui) {
    ui.SetNextWindowPos({330.0f, 60.0f}, Cond::FirstUseEver);
    ui.SetNextWindowSize({460.0f, 430.0f}, Cond::FirstUseEver);
    if (ui.Begin("Settings", &m_showSettings)) {
        if (ui.BeginTabBar("tabs")) {
            if (ui.BeginTabItem("Video")) {
                static const char* const resolutions[] = {"1280 x 720", "1600 x 900", "1920 x 1080", "2560 x 1440",
                                                          "3840 x 2160"};
                static const char* const modes[] = {"Fullscreen", "Borderless", "Windowed"};
                ui.Spacing();
                ui.Combo("Resolution", &m_resolution, resolutions, int(std::size(resolutions)));
                ui.Combo("Window mode", &m_windowMode, modes, int(std::size(modes)));
                ui.Checkbox("Vertical sync", &m_vsync);
                ui.SliderFloat("Brightness", &m_brightness, 0.5f, 1.5f, "%.2f");
                ui.SliderInt("Field of view", &m_fov, 60, 120);
                if (ui.IsItemHovered()) ui.SetTooltip("Horizontal field of view in degrees");
                ui.EndTabItem();
            }
            if (ui.BeginTabItem("Audio")) {
                ui.Spacing();
                ui.SliderFloat("Master", &m_master, 0.0f, 100.0f, "%.0f%%");
                ui.SliderFloat("Music", &m_music, 0.0f, 100.0f, "%.0f%%");
                ui.SliderFloat("Effects", &m_effects, 0.0f, 100.0f, "%.0f%%");
                ui.SliderFloat("Voice", &m_voice, 0.0f, 100.0f, "%.0f%%");
                ui.Checkbox("Subtitles", &m_subtitles);
                ui.EndTabItem();
            }
            if (ui.BeginTabItem("Controls")) {
                ui.Spacing();
                ui.DragFloat("Mouse sensitivity", &m_sensitivity, 0.01f, 0.1f, 5.0f, "%.2f");
                ui.Checkbox("Invert Y axis", &m_invertY);
                ui.RadioButton("Hold to aim", &m_aimMode, 0);
                ui.SameLine();
                ui.RadioButton("Toggle aim", &m_aimMode, 1);
                ui.InputText("Player name", m_playerName, sizeof(m_playerName));
                ui.InputTextWithHint("Server", "address:port", m_server, sizeof(m_server));
                ui.EndTabItem();
            }
            if (ui.BeginTabItem("Theme")) {
                ui.Spacing();
                const int previous = m_theme;
                ui.RadioButton("Dark", &m_theme, 0);
                ui.SameLine();
                ui.RadioButton("Light", &m_theme, 1);
                if (m_theme != previous) {
                    UiStyle& style = ui.Style();
                    UiStyle fresh = m_theme == 0 ? UiStyle::Dark() : UiStyle::Light();
                    fresh.font = style.font;
                    fresh.fontSize = style.fontSize;
                    style = fresh;
                }
                ui.SliderFloat("Window rounding", &ui.Style().windowRounding, 0.0f, 16.0f, "%.0f");
                ui.SliderFloat("Frame rounding", &ui.Style().frameRounding, 0.0f, 12.0f, "%.0f");
                ui.SliderFloat("Shadow size", &ui.Style().windowShadowSize, 0.0f, 40.0f, "%.0f");
                if (ui.Button("Copy theme to clipboard") && ui.Platform().setClipboardText) {
                    ui.Platform().setClipboardText(ui.Platform().userData, ui.Style().SaveTheme().c_str());
                }
                ui.TextDisabled("Themes are plain text: see UiStyle::LoadTheme.");
                ui.EndTabItem();
            }
            ui.EndTabBar();
        }
        ui.Spacing();
        ui.Separator();
        ui.Spacing();
        if (ui.Button("Apply")) m_clicks++;
        ui.SameLine();
        if (ui.Button("Restore defaults")) {
            m_brightness = 1.0f;
            m_fov = 90;
            m_master = 80.0f;
        }
    }
    ui.End();
}

void UiDemo::BuildDebug(Ui& ui, float time, const DemoStats& stats, TextureId image) {
    ui.SetNextWindowPos({810.0f, 60.0f}, Cond::FirstUseEver);
    ui.SetNextWindowSize({430.0f, 560.0f}, Cond::FirstUseEver);
    if (ui.Begin("Debug", &m_showDebug)) {
        ui.TextF("%s  |  %.0f fps", stats.api, double(stats.fps));
        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "frame %.2f ms", double(stats.frameMs[(stats.frameIndex + 119) % 120]));
        ui.SetNextItemWidth(-1.0f);
        ui.PlotLines("##frametime", stats.frameMs, 120, stats.frameIndex, overlay, 0.0f, 20.0f, {0.0f, 56.0f});
        ui.TextF("build %.3f ms  submit %.3f ms  gpu %.3f ms", stats.buildMs, stats.submitMs, stats.gpuMs);
        ui.TextF("%u prims in %u draw calls", stats.prims, stats.drawCalls);

        if (ui.CollapsingHeader("Widgets", TreeNodeFlags::DefaultOpen)) {
            if (ui.Button("Click me")) m_clicks++;
            ui.SameLine();
            ui.TextF("clicked %d times", m_clicks);
            if (ui.BeginPopupContextItem("button menu")) {
                ui.MenuItem("Copy", "Ctrl+C");
                ui.MenuItem("Paste", "Ctrl+V");
                ui.MenuItem("God mode", {}, m_godMode);
                ui.EndPopup();
            }
            ui.Checkbox("God mode", &m_godMode);
            ui.SameLine();
            ui.SmallButton("small");
            ui.DragInt("Spawn count", &m_spawnCount, 0.2f, 0, 100);
            ui.SliderFloat("Time scale", &m_timeScale, 0.0f, 4.0f, "%.2fx");
            static const char* const levels[] = {"Forest", "Desert", "Snow", "Volcano", "Ocean", "Space"};
            ui.Combo("Level", &m_combo, levels, int(std::size(levels)));
            ui.ProgressBar(0.5f + 0.5f * std::sin(time * 0.7f));
            ui.InputText("Console", &m_console, InputTextFlags::EnterReturnsTrue);
            ui.Image(image, {48.0f, 48.0f}, {0, 0}, {1, 1}, colors::White, 8.0f);
            ui.SameLine();
            ui.ImageButton("img", image, {40.0f, 40.0f});
            if (ui.IsItemHovered()) ui.SetTooltip("An image button");
            ui.BeginDisabled();
            ui.Button("Disabled button");
            ui.EndDisabled();
        }
        if (ui.CollapsingHeader("Scene tree")) {
            if (ui.TreeNode("World", TreeNodeFlags::DefaultOpen)) {
                if (ui.TreeNode("Players")) {
                    for (int i = 1; i <= 3; ++i) {
                        char name[32];
                        std::snprintf(name, sizeof(name), "Player %d", i);
                        if (ui.Selectable(name, m_selectedEntity == i)) m_selectedEntity = i;
                    }
                    ui.TreePop();
                }
                if (ui.TreeNode("Static meshes", TreeNodeFlags::Leaf)) ui.TreePop();  // leaves are always "open"
                ui.TreePop();
            }
        }
        if (ui.CollapsingHeader("Log")) {
            if (ui.BeginChild("log", {0.0f, 160.0f}, true)) {
                for (int i = 0; i < 300; ++i) {
                    if (i % 7 == 0) ui.TextColored(Hex(0xFFB35C), "[warn] texture streaming budget exceeded");
                    else ui.TextF("[%04d] tick %d ok", i, i * 16);
                }
            }
            ui.EndChild();
        }
    }
    ui.End();
}

} // namespace sandbox
