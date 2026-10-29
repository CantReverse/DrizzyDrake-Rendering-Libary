#include "demo_drizmenu.h"

#include "demo_fonts.h"

#include <algorithm>
#include <cmath>

using namespace drizzy;

namespace demo {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kFontSize = 12.0f;  // the menu's font size at 1080p (Verdana, like the classic menus)

// This frame's menu scale: the screen resolution (1080p and below = 1) times the Menu scale setting. Every size in
// the menu goes through it (style sizes, font size, layout, icons), so it keeps its proportions and stays crisp at
// any size: nothing is stretched as a bitmap.
float g_scale = 1.0f;
float FontSize() { return kFontSize * g_scale; }
float Px(float v) { return std::floor(v * g_scale); }

// Everything the menu edits. A real mod would read these from its game systems instead.
struct MenuState {
    // Player
    bool godMode = true, noClip = false, infiniteStamina = true, autoSprint = false, thirdPerson = false;
    float walkSpeed = 1.0f, jumpHeight = 1.5f;
    int fov = 90, cameraShake = 1;
    bool infiniteAmmo = true, freeCrafting = false, unlimitedMoney = false, maxCarry = true;
    int giveItem = 0;
    // World
    bool freezeTime = false, pauseAi = false, spawnEnemies = true, spawnLoot = true;
    float timeOfDay = 14.5f, wind = 0.3f, spawnRate = 1.0f, gravity = 1.0f, timeScale = 1.0f;
    int weather = 0;
    // Visuals
    bool colliders = true, hitboxes = false, navMesh = true, aiPaths = true, spawnPoints = false, triggers = false;
    bool healthBars = true, nameTags = true, damageNumbers = false, soundEvents = false, lineOfSight = false;
    bool offscreenArrows = true;
    Color colliderColor = Hex(0xEFEFEF), hitboxColor = Hex(0xE04848), navMeshColor = Hex(0x4C8FD6);
    Color aiPathColor = Hex(0xE0B040), spawnColor = Hex(0xC040D0), healthColor = Hex(0x6FD06F);
    Color nameColor = Hex(0xEFEFEF), sightColor = Hex(0xD02020), arrowColor = Hex(0xEFEFEF);
    bool hlPlayers = true, hlEnemies = true, hlItems = false, hlLoot = true, hlInteractables = false;
    int hlStyle = 0;
    Color hlPlayerColor = Hex(0x4C8FD6), hlEnemyColor = Hex(0xB06030), hlItemColor = Hex(0x7A2D2D);
    Color hlLootColor = Hex(0x2F7FD8), hlInteractColor = Hex(0xE0A020);
    bool crosshair = true, waypoints = true, objectives = true, minimap = false, compass = true, bombTimer = false;
    Color waypointColor = Hex(0xEFEFEF), objectiveColor = Hex(0xEFEFEF);
    bool noFog = true, noMotionBlur = true, noBloom = false, noVignette = true, noScopeOverlay = true;
    int postProcessing = 0, lighting = 1;
    float fogDensity = 50.0f, bloom = 50.0f;  // percent
    // Appearance
    Color outfitColor = Hex(0x3A6EA5), hairColor = Hex(0x4A3020), skinColor = Hex(0xE0B090), eyeColor = Hex(0x3F8F4F);
    Color tracerColor = Rgba(255, 200, 60, 200), trailColor = Rgba(120, 200, 255, 140);
    bool tracers = true, trails = false, glowOutfit = false;
    int outfit = 0, weaponSkin = 2;
    // Misc
    Color menuColor = Hex(0x87B535);
    bool notifications = true, showFps = true, showFrameTime = false, watermark = true;
    int menuKey = 0;
    bool scaleWithResolution = true;  // 1440p: 1.33x, 4K: 2x
    int menuScale = 100;              // percent, on top of the resolution scale
    int menuScaleEdit = 100;          // the slider's value, applied to menuScale when it is let go
    // Config
    int profile = 0;
    char profileName[32] = "default";
};

// `c` mixed toward white by `t` (hover and active shades of the menu color).
Color Lighten(Color c, float t) {
    auto channel = [&](int shift) {
        const float v = float((c >> shift) & 0xFFu);
        return uint32_t(v + (255.0f - v) * t + 0.5f) << shift;
    };
    return channel(0) | channel(8) | channel(16) | (c & 0xFF000000u);
}

// `a` to `b` by `t`, per channel (alpha included).
Color Mix(Color a, Color b, float t) {
    auto channel = [&](int shift) {
        const float x = float((a >> shift) & 0xFFu), y = float((b >> shift) & 0xFFu);
        return uint32_t(x + (y - x) * t + 0.5f) << shift;
    };
    return channel(0) | channel(8) | channel(16) | channel(24);
}

MenuState& State() {
    static MenuState state;
    return state;
}

// A checkbox with a color swatch at the right edge of the row.
void CheckColor(Ui& ui, const char* label, bool* value, Color* color) {
    const float right = ui.GetCursorScreenPos().x + ui.GetContentRegionAvail().x;
    ui.Checkbox(label, value);
    ui.SameLine();
    ui.SetCursorScreenPos({right - std::floor(FontSize() * 1.4f), ui.GetCursorScreenPos().y});
    ui.PushID(label);
    ui.ColorEdit4("##color", color, ColorEditFlags::NoInputs | ColorEditFlags::NoLabel);
    ui.PopID();
}

// A label on its own line with a color swatch at the right edge (no checkbox).
void LabelColor(Ui& ui, const char* label, Color* color) {
    const float right = ui.GetCursorScreenPos().x + ui.GetContentRegionAvail().x;
    ui.Text(label);
    ui.SameLine();
    ui.SetCursorScreenPos({right - std::floor(FontSize() * 1.4f), ui.GetCursorScreenPos().y});
    ui.PushID(label);
    ui.ColorEdit4("##color", color, ColorEditFlags::NoInputs | ColorEditFlags::NoLabel);
    ui.PopID();
}

// Where the group boxes go: two columns right of the tab column, between margins.
struct Columns {
    float left, right, width, top, height, gap;
};

Columns ContentColumns(Ui& ui) {
    const Vec2 start = ui.GetCursorPos(), avail = ui.GetContentRegionAvail();
    const float margin = Px(14.0f), gap = Px(14.0f);
    Columns c;
    c.gap = gap;
    c.width = std::floor((avail.x - margin - gap) * 0.5f);
    c.left = start.x;
    c.right = start.x + c.width + gap;
    c.top = start.y + margin;
    c.height = std::floor(avail.y - margin * 2.0f);
    return c;
}

// A group box at a position in the window, its title in the bold font.
bool Box(Ui& ui, const char* title, float x, float y, float w, float h, const Font* bold) {
    ui.SetCursorPos({x, y});
    ui.PushFont(bold, FontSize());
    const bool open = ui.BeginGroupBox(title, {w, h});
    ui.PopFont();
    return open;
}

// ---- Tab icons, drawn over the tab cells (lengths in 1080p pixels, scaled by Px / g_scale) ------------------------
void IconPlayer(DrawList& dl, Vec2 c, Color col) {
    const float k = g_scale;
    dl.AddCircleFilled({c.x, c.y - 7.0f * k}, 6.0f * k, col);
    dl.PathArcTo({c.x, c.y + 13.0f * k}, 12.0f * k, kPi, kPi * 2.0f);
    dl.PathFillConvex(col);
}

void IconWorld(DrawList& dl, Vec2 c, Color col) {
    const float k = g_scale;
    dl.AddCircle(c, 13.0f * k, col, 2.0f * k);
    dl.AddEllipse(c, {5.5f * k, 13.0f * k}, col, 1.5f * k);
    dl.AddLine({c.x - 13.0f * k, c.y}, {c.x + 13.0f * k, c.y}, col, 1.5f * k);
    dl.AddLine({c.x - 11.0f * k, c.y - 6.5f * k}, {c.x + 11.0f * k, c.y - 6.5f * k}, col, 1.5f * k);
    dl.AddLine({c.x - 11.0f * k, c.y + 6.5f * k}, {c.x + 11.0f * k, c.y + 6.5f * k}, col, 1.5f * k);
}

void IconVisuals(DrawList& dl, Vec2 c, Color col) {
    // A half-lit sun: the left half filled, rays all around.
    const float k = g_scale;
    dl.AddCircle(c, 9.5f * k, col, 2.0f * k);
    dl.PathArcTo(c, 9.5f * k, kPi * 0.5f, kPi * 1.5f);
    dl.PathFillConvex(col);
    for (int i = 0; i < 8; ++i) {
        const float a = kPi * 0.25f * float(i);
        const Vec2 d(std::cos(a), std::sin(a));
        dl.AddLine(c + d * (13.0f * k), c + d * (17.0f * k), col, 2.0f * k);
    }
}

void IconAppearance(DrawList& dl, Vec2 c, Color col) {
    // A paint palette: a ring with four paint dots.
    const float k = g_scale;
    dl.AddCircle(c, 13.0f * k, col, 2.0f * k);
    for (int i = 0; i < 4; ++i) {
        const float a = kPi * (1.0f + 0.3f * float(i));
        dl.AddCircleFilled(c + Vec2(std::cos(a), std::sin(a)) * (7.5f * k), 2.6f * k, col);
    }
    dl.AddCircleFilled(c + Vec2(5.0f, 5.0f) * k, 3.2f * k, col);
}

void IconMisc(DrawList& dl, Vec2 c, Color col) {
    // A gear.
    const float k = g_scale;
    dl.AddCircle(c, 8.5f * k, col, 3.5f * k);
    for (int i = 0; i < 8; ++i) {
        const float a = kPi * 0.25f * float(i) + kPi * 0.125f;
        const Vec2 d(std::cos(a), std::sin(a));
        dl.AddLine(c + d * (9.0f * k), c + d * (13.5f * k), col, 4.0f * k);
    }
}

void IconConfig(DrawList& dl, Vec2 c, Color col) {
    // Three sliders.
    const float k = g_scale;
    const float knobs[3] = {-5.0f, 6.0f, -1.0f};
    for (int i = 0; i < 3; ++i) {
        const float y = c.y + float(i - 1) * 8.0f * k;
        dl.AddLine({c.x - 13.0f * k, y}, {c.x + 13.0f * k, y}, col, 2.0f * k);
        dl.AddCircleFilled({c.x + knobs[i] * k, y}, 3.5f * k, col);
    }
}

// ---- Tabs ------------------------------------------------------------------------------------------------------------
void PlayerTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kShake[] = {"Off", "Reduced", "Full"};
    static const char* const kItems[] = {"Medkit", "Ammo crate", "Lockpick", "Grappling hook", "Night vision"};
    const float top = c.height * 0.56f;
    if (Box(ui, "Movement", c.left, c.top, c.width, top, bold)) {
        ui.Checkbox("God mode", &s.godMode);
        ui.Checkbox("No clip", &s.noClip);
        ui.Checkbox("Infinite stamina", &s.infiniteStamina);
        ui.Checkbox("Auto sprint", &s.autoSprint);
        ui.SliderFloat("Walk speed", &s.walkSpeed, 0.5f, 3.0f, "%.1fx");
        ui.SliderFloat("Jump height", &s.jumpHeight, 0.5f, 5.0f, "%.1f m");
    }
    ui.EndGroupBox();
    if (Box(ui, "Camera", c.left, c.top + top + c.gap, c.width, c.height - top - c.gap, bold)) {
        ui.Checkbox("Third person", &s.thirdPerson);
        ui.SliderInt("Field of view", &s.fov, 60, 120, "%d\xC2\xB0");
        ui.Combo("Camera shake", &s.cameraShake, kShake, 3);
    }
    ui.EndGroupBox();
    if (Box(ui, "Inventory", c.right, c.top, c.width, c.height, bold)) {
        ui.Checkbox("Infinite ammo", &s.infiniteAmmo);
        ui.Checkbox("Free crafting", &s.freeCrafting);
        ui.Checkbox("Unlimited money", &s.unlimitedMoney);
        ui.Checkbox("No carry limit", &s.maxCarry);
        ui.Combo("Give item", &s.giveItem, kItems, 5);
        if (ui.Button("Give", {ui.CalcItemWidth(), 0.0f})) {
            ui.Notify("Added to your inventory.", NotifyType::Success);
        }
    }
    ui.EndGroupBox();
}

void WorldTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kWeather[] = {"Clear", "Rain", "Storm", "Snow", "Fog"};
    const float top = c.height * 0.5f;
    if (Box(ui, "Time and weather", c.left, c.top, c.width, top, bold)) {
        ui.Checkbox("Freeze time", &s.freezeTime);
        ui.SliderFloat("Time of day", &s.timeOfDay, 0.0f, 24.0f, "%.1f h");
        ui.Combo("Weather", &s.weather, kWeather, 5);
        ui.SliderFloat("Wind", &s.wind, 0.0f, 1.0f, "%.2f");
    }
    ui.EndGroupBox();
    if (Box(ui, "Physics", c.left, c.top + top + c.gap, c.width, c.height - top - c.gap, bold)) {
        ui.SliderFloat("Gravity", &s.gravity, 0.1f, 2.0f, "%.2fx");
        ui.SliderFloat("Time scale", &s.timeScale, 0.1f, 2.0f, "%.2fx");
    }
    ui.EndGroupBox();
    if (Box(ui, "Spawning", c.right, c.top, c.width, c.height, bold)) {
        ui.Checkbox("Spawn enemies", &s.spawnEnemies);
        ui.Checkbox("Spawn loot", &s.spawnLoot);
        ui.Checkbox("Pause AI", &s.pauseAi);
        ui.SliderFloat("Spawn rate", &s.spawnRate, 0.0f, 4.0f, "%.1fx");
        if (ui.Button("Clear enemies", {ui.CalcItemWidth(), 0.0f})) ui.Notify("Enemies cleared.");
    }
    ui.EndGroupBox();
}

void VisualsTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kHighlight[] = {"Solid", "Outline", "Glow", "Flat"};
    static const char* const kPost[] = {"Default", "Minimal", "Off"};
    static const char* const kLighting[] = {"Default", "Night mode", "Full bright"};
    const float overlayH = std::floor(c.height * 0.6f);
    if (Box(ui, "Debug overlay", c.left, c.top, c.width, overlayH, bold)) {
        CheckColor(ui, "Colliders", &s.colliders, &s.colliderColor);
        CheckColor(ui, "Hitboxes", &s.hitboxes, &s.hitboxColor);
        CheckColor(ui, "Navigation mesh", &s.navMesh, &s.navMeshColor);
        CheckColor(ui, "AI paths", &s.aiPaths, &s.aiPathColor);
        CheckColor(ui, "Spawn points", &s.spawnPoints, &s.spawnColor);
        ui.Checkbox("Trigger volumes", &s.triggers);
        CheckColor(ui, "Health bars", &s.healthBars, &s.healthColor);
        CheckColor(ui, "Name tags", &s.nameTags, &s.nameColor);
        ui.Checkbox("Damage numbers", &s.damageNumbers);
        ui.Checkbox("Sound events", &s.soundEvents);
        CheckColor(ui, "Line of sight", &s.lineOfSight, &s.sightColor);
        CheckColor(ui, "Off-screen arrows", &s.offscreenArrows, &s.arrowColor);
    }
    ui.EndGroupBox();
    if (Box(ui, "Highlights", c.left, c.top + overlayH + c.gap, c.width, c.height - overlayH - c.gap, bold)) {
        CheckColor(ui, "Players", &s.hlPlayers, &s.hlPlayerColor);
        CheckColor(ui, "Enemies", &s.hlEnemies, &s.hlEnemyColor);
        ui.Combo("##style", &s.hlStyle, kHighlight, 4);
        CheckColor(ui, "Items", &s.hlItems, &s.hlItemColor);
        CheckColor(ui, "Loot", &s.hlLoot, &s.hlLootColor);
        CheckColor(ui, "Interactables", &s.hlInteractables, &s.hlInteractColor);
    }
    ui.EndGroupBox();
    const float hudH = std::floor(c.height * 0.42f);
    if (Box(ui, "HUD", c.right, c.top, c.width, hudH, bold)) {
        ui.Checkbox("Crosshair", &s.crosshair);
        CheckColor(ui, "Waypoints", &s.waypoints, &s.waypointColor);
        CheckColor(ui, "Objective markers", &s.objectives, &s.objectiveColor);
        ui.Checkbox("Minimap", &s.minimap);
        ui.Checkbox("Compass", &s.compass);
        ui.Checkbox("Event timers", &s.bombTimer);
    }
    ui.EndGroupBox();
    if (Box(ui, "Effects", c.right, c.top + hudH + c.gap, c.width, c.height - hudH - c.gap, bold)) {
        ui.Checkbox("Disable fog", &s.noFog);
        ui.Checkbox("Disable motion blur", &s.noMotionBlur);
        ui.Checkbox("Disable bloom", &s.noBloom);
        ui.Checkbox("Disable vignette", &s.noVignette);
        ui.Combo("Post processing", &s.postProcessing, kPost, 3);
        ui.SliderFloat("Fog density", &s.fogDensity, 0.0f, 100.0f, "%.0f%%");
        ui.SliderFloat("Bloom strength", &s.bloom, 0.0f, 100.0f, "%.0f%%");
        ui.Combo("Lighting", &s.lighting, kLighting, 3);
        ui.Checkbox("Disable scope overlay", &s.noScopeOverlay);
    }
    ui.EndGroupBox();
}

void AppearanceTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kOutfits[] = {"Ranger", "Scout", "Engineer", "Medic"};
    static const char* const kSkins[] = {"Factory", "Carbon", "Gold", "Hologram", "Woodland"};
    if (Box(ui, "Character", c.left, c.top, c.width, c.height, bold)) {
        ui.Combo("Outfit", &s.outfit, kOutfits, 4);
        LabelColor(ui, "Outfit color", &s.outfitColor);
        LabelColor(ui, "Hair color", &s.hairColor);
        LabelColor(ui, "Skin tone", &s.skinColor);
        LabelColor(ui, "Eye color", &s.eyeColor);
        ui.Checkbox("Glowing outfit", &s.glowOutfit);
    }
    ui.EndGroupBox();
    if (Box(ui, "Weapon", c.right, c.top, c.width, c.height, bold)) {
        ui.Combo("Weapon skin", &s.weaponSkin, kSkins, 5);
        CheckColor(ui, "Bullet tracers", &s.tracers, &s.tracerColor);
        CheckColor(ui, "Movement trails", &s.trails, &s.trailColor);
        ui.TextDisabled("Right-click a swatch to copy it,");
        ui.TextDisabled("then paste it on another.");
    }
    ui.EndGroupBox();
}

void MiscTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kKeys[] = {"Insert", "Home", "F1", "Right shift"};
    const float top = c.height * 0.5f;
    if (Box(ui, "Menu", c.left, c.top, c.width, top, bold)) {
        LabelColor(ui, "Menu color", &s.menuColor);
        ui.Checkbox("Scale with resolution", &s.scaleWithResolution);
        // Applied when the slider is let go: resizing the menu while dragging would move the slider under the mouse.
        ui.SliderInt("Menu scale", &s.menuScaleEdit, 50, 200, "%d%%");
        if (!ui.IsItemActive()) s.menuScale = s.menuScaleEdit;
        ui.Combo("Menu key", &s.menuKey, kKeys, 4);
        ui.Checkbox("Watermark", &s.watermark);
    }
    ui.EndGroupBox();
    if (Box(ui, "Notifications", c.left, c.top + top + c.gap, c.width, c.height - top - c.gap, bold)) {
        ui.Checkbox("Show notifications", &s.notifications);
        if (ui.Button("Test notification", {ui.CalcItemWidth(), 0.0f}) && s.notifications) {
            ui.Notify("This is a notification.", NotifyType::Info);
        }
    }
    ui.EndGroupBox();
    if (Box(ui, "Performance", c.right, c.top, c.width, c.height, bold)) {
        ui.Checkbox("Show FPS", &s.showFps);
        ui.Checkbox("Show frame time", &s.showFrameTime);
        ui.Spacing();
        ui.TextDisabled("This menu costs about 0.02 ms of CPU");
        ui.TextDisabled("and 0.02 ms of GPU per frame.");
    }
    ui.EndGroupBox();
}

void ConfigTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kProfiles[] = {"default", "speedrun", "screenshots", "testing"};
    if (Box(ui, "Profiles", c.left, c.top, c.width, c.height, bold)) {
        ui.Combo("Profile", &s.profile, kProfiles, 4);
        const Vec2 size(ui.CalcItemWidth(), 0.0f);
        if (ui.Button("Load", size)) ui.Notify("Profile loaded.", NotifyType::Success);
        if (ui.Button("Save", size)) ui.Notify("Profile saved.", NotifyType::Success);
        if (ui.Button("Reset to defaults", size)) {
            State() = MenuState();
            ui.Notify("Settings reset.", NotifyType::Warning);
        }
        ui.InputText("New profile", s.profileName, sizeof(s.profileName));
        if (ui.Button("Create", size)) ui.Notify("Profile created.", NotifyType::Success);
    }
    ui.EndGroupBox();
    if (Box(ui, "About", c.right, c.top, c.width, c.height, bold)) {
        ui.Text("drizzy_renderer");
        ui.TextDisabled("DrizMenu look and theme");
        ui.Spacing();
        ui.TextWrapped("Icon tabs are a vertical tab bar, the panels are group boxes, and the colors come from "
                       "UiTheme::DrizMenu.");
    }
    ui.EndGroupBox();
}

} // namespace

std::vector<uint32_t> CarbonPixels(uint32_t size) {
    // Light (0x15) and dark (0x0E) greys in a 4x4 tile: every other column dark on even rows, one in four on odd rows.
    static const uint8_t kTile[4][4] = {
        {0x15, 0x0E, 0x15, 0x0E},
        {0x15, 0x0E, 0x15, 0x15},
        {0x15, 0x0E, 0x15, 0x0E},
        {0x15, 0x15, 0x15, 0x0E},
    };
    std::vector<uint32_t> pixels(size_t(size) * size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const uint32_t v = kTile[y & 3][x & 3];
            pixels[size_t(y) * size + x] = 0xFF000000u | (v << 16) | (v << 8) | v;
        }
    }
    return pixels;
}

void ShowDrizMenu(Ui& ui, TextureId carbon) {
    MenuState& s = State();
    const SystemFonts& fonts = DemoSystemFonts();
    const Vec2 display = ui.GetDisplaySize();
    const float resolutionScale = s.scaleWithResolution ? std::max(1.0f, display.y / 1080.0f) : 1.0f;
    const float previousScale = g_scale;
    g_scale = std::clamp(resolutionScale * float(s.menuScale) / 100.0f, 0.5f, 4.0f);
    const UiStyle saved = ui.Style();
    UiStyle& style = ui.Style();
    style.ApplyLook(UiLook::DrizMenu);
    style.ApplyTheme(UiTheme::DrizMenu);
    style.ScaleAllSizes(g_scale);  // one-pixel lines stay one pixel (sizes round down)
    // The menu color (Misc > Menu color) recolors every accent.
    style.colors[size_t(UiColor::CheckMark)] = s.menuColor;
    style.colors[size_t(UiColor::SliderGrab)] = s.menuColor;
    style.colors[size_t(UiColor::SliderGrabActive)] = Lighten(s.menuColor, 0.15f);
    const Font* regular = fonts.verdana ? fonts.verdana : saved.font;
    const Font* bold = fonts.verdanaBold ? fonts.verdanaBold : regular;
    ui.PushFont(regular, FontSize());

    ui.SetNextWindowPos({std::floor(display.x * 0.5f), std::floor(display.y * 0.5f)}, Cond::FirstUseEver, {0.5f, 0.5f});
    // The classic 660 x 560 at 1080p, resized with the scale (after a resolution or Menu scale change too).
    ui.SetNextWindowSize({Px(660.0f), Px(560.0f)}, g_scale != previousScale ? Cond::Always : Cond::FirstUseEver);
    ui.PushStyleVar(UiStyleVar::WindowPadding, Vec2(0.0f, 0.0f));
    const bool visible = ui.Begin("DrizMenu", nullptr, WindowFlags::NoTitleBar | WindowFlags::NoScrollbar);
    ui.PopStyleVar();
    if (visible) {
        DrawList& dl = ui.WindowDrawList();
        if (carbon != kNoTexture) {
            // One texel per pixel, tiled under everything (the content area clips it).
            const Vec2 pos = ui.GetWindowPos(), size = ui.GetWindowSize();
            const float tile = 256.0f;  // texels stay one per pixel at any scale: the weave stays crisp
            const Color tint = ScaleAlpha(colors::White, ui.GetAlpha());  // fades with the menu
            for (float y = pos.y; y < pos.y + size.y; y += tile) {
                for (float x = pos.x; x < pos.x + size.x; x += tile) {
                    dl.AddImage(carbon, Rect(x, y, x + tile, y + tile), {0.0f, 0.0f}, {1.0f, 1.0f}, tint);
                }
            }
        }
        using IconFn = void (*)(DrawList&, Vec2, Color);
        using TabFn = void (*)(Ui&, MenuState&, const Font*);
        struct Tab {
            const char* id;
            IconFn icon;
            TabFn content;
        };
        static const Tab kTabs[] = {
            {"##player", IconPlayer, PlayerTab},   {"##world", IconWorld, WorldTab},
            {"##visuals", IconVisuals, VisualsTab}, {"##appearance", IconAppearance, AppearanceTab},
            {"##misc", IconMisc, MiscTab},         {"##config", IconConfig, ConfigTab},
        };
        if (ui.BeginTabBar("##tabs", TabBarFlags::Vertical, {Px(76.0f), Px(78.0f)})) {
            for (const Tab& tab : kTabs) {
                // True for the selected tab, and for a moment after a switch also for the one sliding out.
                const bool show = ui.BeginTabItem(tab.id);
                const Rect cell(ui.GetItemRectMin(), ui.GetItemRectMax());
                // Icons brighten under the mouse over ~60 ms, like the library's own tab labels.
                const float hover = ui.Animate(tab.id, ui.IsItemHovered() ? 1.0f : 0.0f, 0.06f);
                const Color color = ui.IsTabItemSelected()
                                        ? ui.GetColor(UiColor::Text, 0.85f)
                                        : Mix(ui.GetColor(UiColor::TextDisabled), ui.GetColor(UiColor::Text, 0.85f), hover);
                tab.icon(dl, {std::floor(cell.Center().x) - 1.0f, std::floor(cell.Center().y)}, color);
                if (show) {
                    tab.content(ui, s, bold);
                    ui.EndTabItem();
                }
            }
            ui.EndTabBar();
        }
    }
    ui.End();
    ui.PopFont();
    ui.Style() = saved;
}

} // namespace demo
