// A game-style UI demo built only from the public Ui API: main menu, settings, debug tools and a HUD over an
// animated "game world" drawn with the background draw list.
#pragma once

#include "drizzy/ui.h"

#include <string>

namespace sandbox {

struct DemoStats {
    const char* api = "";
    float fps = 0.0f;
    float frameMs[120] = {};  // ring buffer of recent frame times
    int frameIndex = 0;
    uint32_t prims = 0, drawCalls = 0;
    double buildMs = 0.0, submitMs = 0.0, gpuMs = 0.0;

    void PushFrameTime(float ms) {
        frameMs[frameIndex] = ms;
        frameIndex = (frameIndex + 1) % 120;
    }
};

class UiDemo {
public:
    void Build(drizzy::Ui& ui, float time, const DemoStats& stats, drizzy::TextureId image);
    bool quitRequested = false;
    bool* keybindsWindow = nullptr;  // when set, the main menu gets a button that toggles it

private:
    void BuildWorld(drizzy::Ui& ui, float time);
    void BuildMainMenu(drizzy::Ui& ui);
    void BuildSettings(drizzy::Ui& ui);
    void BuildDebug(drizzy::Ui& ui, float time, const DemoStats& stats, drizzy::TextureId image);

    bool m_showSettings = true;
    bool m_showDebug = true;
    // Settings
    int m_resolution = 2;
    int m_windowMode = 0;
    bool m_vsync = true;
    float m_brightness = 1.0f;
    int m_fov = 90;
    float m_master = 80.0f, m_music = 60.0f, m_effects = 90.0f, m_voice = 100.0f;
    bool m_subtitles = true;
    float m_sensitivity = 1.25f;
    bool m_invertY = false;
    int m_aimMode = 0;
    char m_playerName[32] = "Drizzy";
    char m_server[64] = "";
    int m_theme = 0;
    // Debug gallery
    bool m_godMode = false;
    int m_spawnCount = 12;
    float m_timeScale = 1.0f;
    int m_selectedEntity = 1;
    int m_combo = 0;
    std::string m_console;
    int m_clicks = 0;
};

} // namespace sandbox
