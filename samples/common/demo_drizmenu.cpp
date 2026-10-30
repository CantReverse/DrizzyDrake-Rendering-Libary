#include "demo_drizmenu.h"

#include "demo_backdrops.h"
#include "demo_fonts.h"
#include "demo_gallery.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

using namespace drizzy;

namespace demo {
namespace {

constexpr float kPi = 3.14159265f;
constexpr float kFontSize = 12.0f;  // the menu's font size at 1080p (Verdana, like the classic menus)

// This frame's menu scale: the screen resolution (1080p and below = 1) times the Menu scale setting. Every size in
// the menu goes through it (style sizes, font size, layout, icons), so it keeps its proportions and stays crisp at
// any size: nothing is stretched as a bitmap.
float g_scale = 1.0f;
float g_fontSize = kFontSize;  // this frame's font size: the chosen font, at the menu's scale
const Font* g_regularFont = nullptr;  // this frame's regular (not bold) font
float FontSize() { return g_fontSize; }
float Px(float v) { return std::floor(v * g_scale); }

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

// ---- The menu's own look, edited on its Customize tab ---------------------------------------------------------------
// The widget shapes it offers, the DrizMenu look's own first.
const char* const kCheckNames[] = {"Small box", "Check mark", "Filled box", "Square"};
const CheckShape kCheckValues[] = {CheckShape::Small, CheckShape::Check, CheckShape::Fill, CheckShape::Square};
const char* const kSliderNames[] = {"Thin bar with steps", "Thin bar", "Block", "Fill", "Rail", "Segments"};
const SliderShape kSliderValues[] = {SliderShape::BarSteps, SliderShape::Bar,  SliderShape::Block,
                                     SliderShape::Fill,     SliderShape::Rail, SliderShape::Segments};
const char* const kComboNames[] = {"Flush list", "Arrow box"};
const ComboShape kComboValues[] = {ComboShape::Flush, ComboShape::Box};
const char* const kPickerNames[] = {"Compact", "Full"};
const ColorPickerShape kPickerValues[] = {ColorPickerShape::Compact, ColorPickerShape::Full};
const char* const kLabelNames[] = {"Above", "Beside"};
const LabelPlacement kLabelValues[] = {LabelPlacement::Above, LabelPlacement::Right};
// How the menu opens and closes (Customize > Animation). Fade is the classic menu's own, measured frame by frame: its
// opacity eases out over about 85 ms, the same curve backwards to close.
enum OpenStyle : int { kOpenFade, kOpenSlideDown, kOpenSlideUp, kOpenSlideLeft, kOpenSlideRight, kOpenUnroll, kOpenInstant };
const char* const kOpenNames[] = {"Fade",   "Slide down", "Slide up", "Slide in from the left", "Slide in from the right",
                                  "Unroll", "Instant"};
// The keys that can open and close the menu (Misc > Menu key).
const char* const kMenuKeyNames[] = {"Insert", "Home", "End", "Delete"};
const Key kMenuKeys[] = {Key::Insert, Key::Home, Key::End, Key::Delete};

// Lengths are in 1080p pixels (scaled with the menu); percentages are ints so the sliders show whole numbers.
struct MenuSkin {
    Color colors[size_t(UiColor::Count)];  // the theme, before the accent and the panel opacity go over it
    Color accent;
    int font = -1;        // index into Gallery().fonts; -1: Verdana (when the system has it)
    int fontScale = 100;  // percent
    int rounding = 0;
    int shade = 30;       // percent
    int frame = 6;        // the window frame's width
    int stripe = 2;       // the window stripe's height
    int checkShape = 0, sliderShape = 0, comboShape = 0, pickerShape = 0, labels = 0;  // into the lists above
    // Animation, each kind on its own (ms; the DrizMenu look's timings, measured from footage of a classic menu).
    bool animations = true;
    int tabSwitchMs = 150, hoverMs = 60, toggleMs = 90, sliderMs = 90, dropdownMs = 140, popupMs = 75;
    int openStyle = kOpenFade;    // how the menu opens and closes
    int openMs = 85;              // and how long it takes (DrizMenuFadeTime)
    float stripeLoop = 6.0f;      // seconds the stripe's colors take to flow along it and back; 0 = still
    bool carbon = true;  // the carbon-fiber weave over the background color
    int effect = int(BackdropKind::Constellation);  // an animated background, over the weave and under the panels
    Color effectColor = Rgba(213, 213, 213, 150);
    int effectOpacity = 100, effectSpeed = 100, effectDensity = 100;  // percent
    int panelOpacity = 100;                                           // percent
    int tabColumnOpacity = 100;                                       // percent
    Backdrop backdrop;

    MenuSkin() { ResetColors(); }
    void ResetColors() {
        const UiStyle theme = UiStyle::Make(UiTheme::DrizMenu, UiLook::DrizMenu);
        std::memcpy(colors, theme.colors, sizeof(colors));
        // The weave darkens the background by a third in its dark threads: #151515 gives the classic #151515 and
        // #0E0E0E (CarbonPixels).
        colors[size_t(UiColor::WindowBg)] = Hex(0x151515);
        accent = theme.colors[size_t(UiColor::CheckMark)];
        highlight = Lighten(accent, 0.3f);
    }
    Color highlight;  // the labels of options that stand out
};

// The colors the Customize tab offers, besides the accent (every checked, filled and selected widget).
struct ColorSlot {
    const char* name;
    UiColor color;
};
const ColorSlot kColorSlots[] = {
    {"Text", UiColor::Text},       {"Dim text", UiColor::TextDisabled}, {"Background", UiColor::WindowBg},
    {"Panels", UiColor::ChildBg},  {"Fields", UiColor::FrameBg},        {"Buttons", UiColor::Button},
    {"Tab column", UiColor::Tab},  {"Window frame", UiColor::TitleBg},  {"Lists and pickers", UiColor::PopupBg},
    {"Outlines", UiColor::Border}, {"Lines", UiColor::Separator},
};

// After a color is edited: the hover and press shades made from it.
void DeriveShades(MenuSkin& k, UiColor edited) {
    Color* c = k.colors;
    const Color v = c[size_t(edited)];
    switch (edited) {
    case UiColor::FrameBg:
        c[size_t(UiColor::FrameBgHovered)] = Lighten(v, 0.03f);
        c[size_t(UiColor::FrameBgActive)] = Lighten(v, 0.055f);
        break;
    case UiColor::Button:
        c[size_t(UiColor::ButtonHovered)] = Lighten(v, 0.035f);
        c[size_t(UiColor::ButtonActive)] = Mix(v, WithAlpha(colors::Black, ColorAlpha(v)), 0.2f);
        break;
    case UiColor::Tab: c[size_t(UiColor::TabHovered)] = Lighten(v, 0.016f); break;
    default: break;
    }
}

// Sets `style` to the skin: the DrizMenu look with the skin's shapes and sizes at the menu's scale, and the skin's
// colors with the accent and the panel opacity applied.
void ApplySkin(UiStyle& style, const MenuSkin& k) {
    style.ApplyLook(UiLook::DrizMenu);
    style.frameRounding = style.grabRounding = style.popupRounding = float(k.rounding);
    style.shade = float(k.shade) / 100.0f;
    style.windowBorderSize = float(k.frame);
    style.windowStripe = float(k.stripe);
    style.checkShape = kCheckValues[k.checkShape];
    style.sliderShape = kSliderValues[k.sliderShape];
    style.comboShape = kComboValues[k.comboShape];
    style.colorPickerShape = kPickerValues[k.pickerShape];
    style.labelPlacement = kLabelValues[k.labels];
    auto seconds = [&](int ms) { return k.animations ? float(ms) / 1000.0f : 0.0f; };
    style.tabSwitchTime = seconds(k.tabSwitchMs);
    style.hoverTime = seconds(k.hoverMs);
    style.toggleTime = seconds(k.toggleMs);
    style.sliderTime = seconds(k.sliderMs);
    style.dropdownTime = seconds(k.dropdownMs);
    style.popupTime = seconds(k.popupMs);
    style.windowStripeTime = k.animations ? k.stripeLoop : 0.0f;
    style.ScaleAllSizes(g_scale);  // one-pixel lines stay one pixel (sizes round down)
    Color* c = style.colors;
    std::memcpy(c, k.colors, sizeof(k.colors));
    for (UiColor accented : {UiColor::CheckMark, UiColor::SliderGrab, UiColor::PlotLines, UiColor::PlotHistogram}) {
        c[size_t(accented)] = k.accent;
    }
    c[size_t(UiColor::SliderGrabActive)] = Lighten(k.accent, 0.15f);
    c[size_t(UiColor::Glow)] = WithAlpha(k.accent, 0x90);
    c[size_t(UiColor::TextSelectedBg)] = WithAlpha(k.accent, 0x60);
    c[size_t(UiColor::ResizeGripHovered)] = WithAlpha(k.accent, 0xA0);
    c[size_t(UiColor::ResizeGripActive)] = WithAlpha(k.accent, 0xE0);
    c[size_t(UiColor::ChildBg)] = ScaleAlpha(c[size_t(UiColor::ChildBg)], float(k.panelOpacity) / 100.0f);
    for (UiColor column : {UiColor::Tab, UiColor::TabHovered}) {
        c[size_t(column)] = ScaleAlpha(c[size_t(column)], float(k.tabColumnOpacity) / 100.0f);
    }
}

// The skin's font: its index in Gallery().fonts, or -1 (no font list, or no Verdana in it).
int SkinFontIndex(const MenuSkin& k) {
    const std::vector<DemoFont>& list = Gallery().fonts;
    if (k.font >= 0 && k.font < int(list.size())) return k.font;
    const Font* verdana = DemoSystemFonts().verdana;
    for (size_t i = 0; i < list.size(); ++i) {
        if (verdana && list[i].font == verdana) return int(i);
    }
    return -1;
}

struct MenuFonts {
    const Font* regular;
    const Font* bold;  // group box titles
    float size;
};

// The skin's font at the menu's scale. Other fonts are sized to match Verdana, and pixel fonts (Monocraft, Press
// Start 2P) to whole multiples of their pixel grid, so their pixels stay sharp squares.
MenuFonts SkinFonts(const MenuSkin& k, const Font* fallback) {
    const SystemFonts& system = DemoSystemFonts();
    MenuFonts f{system.verdana ? system.verdana : fallback, system.verdanaBold, kFontSize};
    float pixelsPerEm = 0.0f;
    const int index = SkinFontIndex(k);
    if (index >= 0 && Gallery().fonts[size_t(index)].font != system.verdana) {
        const DemoFont& d = Gallery().fonts[size_t(index)];
        f.regular = d.font;
        f.bold = nullptr;
        f.size = d.size * (kFontSize / 13.0f);  // DemoFont sizes match Verdana's at 13
        pixelsPerEm = d.pixelsPerEm;
    }
    f.size *= g_scale * float(k.fontScale) / 100.0f;
    if (pixelsPerEm > 0.0f) f.size = pixelsPerEm * std::max(1.0f, std::floor(f.size / pixelsPerEm + 0.25f));
    if (!f.bold) f.bold = f.regular;
    return f;
}

// A slider whose value is applied when it is let go, for settings that resize the menu (which would move the slider
// under the mouse while it is dragged). Only one slider is dragged at a time, so one value in flight is enough.
void SliderIntOnRelease(Ui& ui, const char* label, int* value, int min, int max, const char* format) {
    static int dragged = 0;
    static const int* target = nullptr;
    int v = target == value ? dragged : *value;
    ui.SliderInt(label, &v, min, max, format);
    if (ui.IsItemActive()) {
        target = value;
        dragged = v;
    } else if (target == value) {
        *value = v;
        target = nullptr;
    }
}

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
    // Hotkeys of the options that have one: a Key, kMouseRightKey or kMouseMiddleKey; -1 for none.
    int godModeKey = -1, noClipKey = int(Key::C), autoSprintKey = -1, thirdPersonKey = int(Key::X);
    int freezeTimeKey = -1, pauseAiKey = int(Key::Z);
    // Misc
    bool notifications = true, showFps = true, showFrameTime = false, watermark = true;  // watermark: the info bar
    int menuKey = 0;  // into kMenuKeys
    // Customize
    MenuSkin skin;
    bool scaleWithResolution = true;  // 1440p: 1.33x, 4K: 2x
    int menuScale = 100;              // percent, on top of the resolution scale
    // Config
    int profile = 0;
    char profileName[32] = "default";
};

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

// ---- Hotkeys ------------------------------------------------------------------------------------------------------
constexpr int kMouseRightKey = 100, kMouseMiddleKey = 101;
int* g_listening = nullptr;  // the hotkey waiting for a key to be pressed (its tag reads "[...]")

const char* KeyName(int key) {
    static const char* const kNames[] = {"TAB", "LEFT", "RIGHT", "UP",  "DOWN",  "PGUP", "PGDN", "HOME", "END", "INS", "DEL",
                                         "BKSP", "SPACE", "ENTER", "ESC", "A", "C", "V", "X", "Y", "Z"};
    static_assert(sizeof(kNames) / sizeof(kNames[0]) == size_t(Key::Count), "one name per Key");
    if (key >= 0 && key < int(Key::Count)) return kNames[key];
    return key == kMouseRightKey ? "M2" : key == kMouseMiddleKey ? "M3" : "-";
}

bool HotkeyPressed(Ui& ui, int key) {
    if (key >= 0 && key < int(Key::Count)) return ui.IsKeyPressed(Key(key), false);
    if (ui.Output().wantCaptureMouse) return false;  // a click on the menu is the menu's
    return (key == kMouseRightKey && ui.IsMouseClicked(MouseButton::Right)) ||
           (key == kMouseMiddleKey && ui.IsMouseClicked(MouseButton::Middle));
}

// A checkbox with its hotkey's tag at the right edge of the row, like "[V]" ("[-]" with none). Click the tag, then press
// a key (or the right or middle mouse button) to bind it; Escape clears it. `highlight` draws the label in the
// highlight color, for options that stand out.
void CheckKey(Ui& ui, const char* label, bool* value, int* key, bool highlight = false) {
    const float right = ui.GetCursorScreenPos().x + ui.GetContentRegionAvail().x;
    if (highlight) ui.PushStyleColor(UiColor::Text, State().skin.highlight);
    ui.Checkbox(label, value);
    if (highlight) ui.PopStyleColor();
    ui.SameLine();
    char tag[16];
    std::snprintf(tag, sizeof(tag), "[%s]", g_listening == key ? "..." : KeyName(*key));
    const float size = std::floor(FontSize() * 0.8f);
    const float rowH = std::floor(g_regularFont->LineHeight() * FontSize() + 0.5f);
    const float tagH = std::floor(g_regularFont->LineHeight() * size + 0.5f);
    const float width = std::ceil(g_regularFont->MeasureText(tag, size).x);
    const Vec2 pos(right - width, ui.GetCursorScreenPos().y + std::floor((rowH - tagH) * 0.5f));
    ui.SetCursorScreenPos(pos);
    ui.PushID(label);
    if (ui.InvisibleButton("##key", {width, tagH})) g_listening = key;
    const bool hovered = ui.IsItemHovered();
    ui.PopID();
    const Color color = g_listening == key || hovered ? ui.GetColor(UiColor::Text) : ui.GetColor(UiColor::TextDisabled);
    ui.WindowDrawList().AddText(*g_regularFont, size, pos, color, tag);
}

// Each frame: a hotkey waiting for a key takes the next one pressed; otherwise bound keys toggle their options.
void UpdateHotkeys(Ui& ui, MenuState& s, bool toggle) {
    if (g_listening) {
        for (int k = 0; k < int(Key::Count); ++k) {
            if (!ui.IsKeyPressed(Key(k), false)) continue;
            *g_listening = Key(k) == Key::Escape ? -1 : k;
            g_listening = nullptr;
            return;
        }
        if (ui.IsMouseClicked(MouseButton::Right) || ui.IsMouseClicked(MouseButton::Middle)) {
            *g_listening = ui.IsMouseClicked(MouseButton::Right) ? kMouseRightKey : kMouseMiddleKey;
            g_listening = nullptr;
        } else if (ui.IsMouseClicked(MouseButton::Left)) {
            g_listening = nullptr;  // a click anywhere else gives up
        }
        return;
    }
    if (!toggle || ui.Output().wantTextInput) return;
    struct Bind {
        int key;
        bool* value;
    };
    const Bind binds[] = {{s.godModeKey, &s.godMode},       {s.noClipKey, &s.noClip},         {s.autoSprintKey, &s.autoSprint},
                          {s.thirdPersonKey, &s.thirdPerson}, {s.freezeTimeKey, &s.freezeTime}, {s.pauseAiKey, &s.pauseAi}};
    for (const Bind& b : binds) {
        if (b.key >= 0 && HotkeyPressed(ui, b.key)) *b.value = !*b.value;
    }
}

// A label on its own line with a color swatch at the right edge (no checkbox). True when the color was changed.
bool LabelColor(Ui& ui, const char* label, Color* color) {
    const float right = ui.GetCursorScreenPos().x + ui.GetContentRegionAvail().x;
    ui.Text(label);
    ui.SameLine();
    ui.SetCursorScreenPos({right - std::floor(FontSize() * 1.4f), ui.GetCursorScreenPos().y});
    ui.PushID(label);
    const bool changed = ui.ColorEdit4("##color", color, ColorEditFlags::NoInputs | ColorEditFlags::NoLabel);
    ui.PopID();
    return changed;
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

void IconCustomize(DrawList& dl, Vec2 c, Color col) {
    // A paint brush: a slanted handle, a ferrule, and a rounded tip with a drop under it.
    const float k = g_scale;
    dl.AddLine({c.x + 12.0f * k, c.y - 12.0f * k}, {c.x + 2.0f * k, c.y - 2.0f * k}, col, 3.0f * k);
    dl.AddLine({c.x + 3.0f * k, c.y - 3.0f * k}, {c.x - 1.0f * k, c.y + 1.0f * k}, col, 5.0f * k);
    dl.AddCircleFilled({c.x - 4.5f * k, c.y + 4.5f * k}, 4.5f * k, col);
    dl.AddTriangleFilled({c.x - 9.0f * k, c.y + 4.5f * k}, {c.x - 4.5f * k, c.y + 9.0f * k}, {c.x - 12.0f * k, c.y + 12.0f * k}, col);
}

void IconConfig(DrawList& dl, Vec2 c, Color col) {
    // A floppy disk: the case, the metal shutter at the top and the label at the bottom.
    const float k = g_scale;
    dl.AddRect(Rect(c.x - 12.0f * k, c.y - 12.0f * k, c.x + 12.0f * k, c.y + 12.0f * k), col, 2.5f * k, 2.0f * k);
    dl.AddRectFilled(Rect(c.x - 6.0f * k, c.y - 12.0f * k, c.x + 6.0f * k, c.y - 4.0f * k), col);
    dl.AddRect(Rect(c.x - 7.5f * k, c.y + 1.0f * k, c.x + 7.5f * k, c.y + 12.0f * k), col, 0.0f, 1.5f * k);
}

// ---- Tabs ------------------------------------------------------------------------------------------------------------
void PlayerTab(Ui& ui, MenuState& s, const Font* bold) {
    const Columns c = ContentColumns(ui);
    static const char* const kShake[] = {"Off", "Reduced", "Full"};
    static const char* const kItems[] = {"Medkit", "Ammo crate", "Lockpick", "Grappling hook", "Night vision"};
    const float top = c.height * 0.56f;
    if (Box(ui, "Movement", c.left, c.top, c.width, top, bold)) {
        CheckKey(ui, "God mode", &s.godMode, &s.godModeKey, true);
        CheckKey(ui, "No clip", &s.noClip, &s.noClipKey, true);
        ui.Checkbox("Infinite stamina", &s.infiniteStamina);
        CheckKey(ui, "Auto sprint", &s.autoSprint, &s.autoSprintKey);
        ui.SliderFloat("Walk speed", &s.walkSpeed, 0.5f, 3.0f, "%.1fx");
        ui.SliderFloat("Jump height", &s.jumpHeight, 0.5f, 5.0f, "%.1f m");
    }
    ui.EndGroupBox();
    if (Box(ui, "Camera", c.left, c.top + top + c.gap, c.width, c.height - top - c.gap, bold)) {
        CheckKey(ui, "Third person", &s.thirdPerson, &s.thirdPersonKey);
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
        CheckKey(ui, "Freeze time", &s.freezeTime, &s.freezeTimeKey);
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
        CheckKey(ui, "Pause AI", &s.pauseAi, &s.pauseAiKey);
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
        ui.BeginDisabled(!s.healthBars);  // dimmed: it needs Health bars
        ui.Checkbox("Damage numbers", &s.damageNumbers);
        ui.EndDisabled();
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
    const float top = c.height * 0.5f;
    if (Box(ui, "Menu", c.left, c.top, c.width, top, bold)) {
        LabelColor(ui, "Menu color", &s.skin.accent);
        ui.Combo("Menu key", &s.menuKey, kMenuKeyNames, 4);
        ui.Checkbox("Info bar", &s.watermark);
        ui.Spacing();
        ui.TextDisabled("Everything else about the menu's");
        ui.TextDisabled("look is on the Customize tab.");
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

void CustomizeTab(Ui& ui, MenuState& s, const Font* bold) {
    MenuSkin& k = s.skin;
    const Columns c = ContentColumns(ui);
    const float colorsH = std::floor(c.height * 0.56f);
    if (Box(ui, "Colors", c.left, c.top, c.width, colorsH, bold)) {
        LabelColor(ui, "Accent", &k.accent);
        LabelColor(ui, "Highlighted options", &k.highlight);
        for (const ColorSlot& slot : kColorSlots) {
            if (LabelColor(ui, slot.name, &k.colors[size_t(slot.color)])) DeriveShades(k, slot.color);
        }
        // The window stripe's three colors, left to right, on one row.
        const float right = ui.GetCursorScreenPos().x + ui.GetContentRegionAvail().x;
        ui.Text("Window stripe");
        const float swatch = std::floor(FontSize() * 1.4f), gap = Px(4.0f);
        for (int i = 0; i < 3; ++i) {
            ui.SameLine();
            ui.SetCursorScreenPos({right - swatch * float(3 - i) - gap * float(2 - i), ui.GetCursorScreenPos().y});
            ui.PushID(i);
            ui.ColorEdit4("##stripe", &k.colors[size_t(UiColor::StripeStart) + size_t(i)],
                          ColorEditFlags::NoInputs | ColorEditFlags::NoLabel);
            ui.PopID();
        }
        if (ui.Button("Reset colors", {ui.CalcItemWidth(), 0.0f})) k.ResetColors();
    }
    ui.EndGroupBox();
    if (Box(ui, "Background", c.left, c.top + colorsH + c.gap, c.width, c.height - colorsH - c.gap, bold)) {
        ui.Checkbox("Carbon fiber", &k.carbon);
        static const char* effects[int(BackdropKind::Count)];
        for (int i = 0; i < int(BackdropKind::Count); ++i) effects[i] = BackdropName(BackdropKind(i));
        ui.Combo("Effect", &k.effect, effects, int(BackdropKind::Count));
        ui.BeginDisabled(k.effect == int(BackdropKind::None));
        LabelColor(ui, "Effect color", &k.effectColor);
        ui.SliderInt("Effect opacity", &k.effectOpacity, 0, 100, "%d%%");
        ui.SliderInt("Effect speed", &k.effectSpeed, 0, 300, "%d%%");
        ui.SliderInt("Effect density", &k.effectDensity, 25, 300, "%d%%");
        ui.EndDisabled();
        ui.SliderInt("Panel opacity", &k.panelOpacity, 0, 100, "%d%%");
        ui.SliderInt("Tab column opacity", &k.tabColumnOpacity, 0, 100, "%d%%");
    }
    ui.EndGroupBox();

    const float styleH = std::floor(c.height * 0.5f);
    if (Box(ui, "Style", c.right, c.top, c.width, styleH, bold)) {
        const std::vector<DemoFont>& fonts = Gallery().fonts;
        const int current = SkinFontIndex(k);
        if (ui.BeginCombo("Font", current >= 0 ? fonts[size_t(current)].name : "Default")) {
            for (int i = 0; i < int(fonts.size()); ++i) {
                ui.PushID(i);
                if (ui.Selectable(fonts[size_t(i)].name, i == current)) k.font = i;
                ui.PopID();
            }
            ui.EndCombo();
        }
        // These resize the menu: applied when the slider is let go.
        SliderIntOnRelease(ui, "Font size", &k.fontScale, 70, 160, "%d%%");
        SliderIntOnRelease(ui, "Menu scale", &s.menuScale, 50, 200, "%d%%");
        ui.Checkbox("Scale with resolution", &s.scaleWithResolution);
        ui.Combo("Checkboxes", &k.checkShape, kCheckNames, 4);
        ui.Combo("Sliders", &k.sliderShape, kSliderNames, 6);
        ui.Combo("Dropdowns", &k.comboShape, kComboNames, 2);
        ui.Combo("Color pickers", &k.pickerShape, kPickerNames, 2);
        ui.Combo("Labels", &k.labels, kLabelNames, 2);
        ui.SliderInt("Rounding", &k.rounding, 0, 8, "%d px");
        ui.SliderInt("Shading", &k.shade, 0, 100, "%d%%");
        SliderIntOnRelease(ui, "Window frame", &k.frame, 2, 10, "%d px");
        SliderIntOnRelease(ui, "Window stripe", &k.stripe, 0, 6, "%d px");
    }
    ui.EndGroupBox();
    if (Box(ui, "Animation", c.right, c.top + styleH + c.gap, c.width, c.height - styleH - c.gap, bold)) {
        ui.Checkbox("Animations", &k.animations);
        ui.BeginDisabled(!k.animations);
        ui.SliderInt("Tab switch", &k.tabSwitchMs, 0, 500, "%d ms");
        ui.SliderInt("Hover", &k.hoverMs, 0, 300, "%d ms");
        ui.SliderInt("Checkboxes##anim", &k.toggleMs, 0, 400, "%d ms");
        ui.SliderInt("Sliders##anim", &k.sliderMs, 0, 400, "%d ms");
        ui.SliderInt("Dropdowns##anim", &k.dropdownMs, 0, 400, "%d ms");
        ui.SliderInt("Popups and tooltips", &k.popupMs, 0, 400, "%d ms");
        ui.Combo("Open and close", &k.openStyle, kOpenNames, 7);
        ui.SliderInt("Open and close time", &k.openMs, 0, 500, "%d ms");
        ui.SliderFloat("Stripe flow", &k.stripeLoop, 0.0f, 20.0f, k.stripeLoop > 0.0f ? "%.1f s a loop" : "Still");
        ui.EndDisabled();
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
        ui.TextWrapped("Icon tabs are a vertical tab bar, the panels are group boxes, and the colors start from "
                       "UiTheme::DrizMenu. The Customize tab restyles all of it.");
    }
    ui.EndGroupBox();
}

} // namespace

std::vector<uint32_t> CarbonPixels(uint32_t size) {
    // Light and dark threads in a 4x4 tile: every other column dark on even rows, one in four on odd rows. Light ones
    // are clear, dark ones black at a third, so the weave shows over any background color (over #151515: the classic
    // #151515 and #0E0E0E).
    static const uint8_t kTile[4][4] = {
        {0, 1, 0, 1},
        {0, 1, 0, 0},
        {0, 1, 0, 1},
        {0, 0, 0, 1},
    };
    std::vector<uint32_t> pixels(size_t(size) * size);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) pixels[size_t(y) * size + x] = kTile[y & 3][x & 3] ? 0x55000000u : 0u;
    }
    return pixels;
}

float DrizMenuFadeTime() {
    const MenuSkin& k = State().skin;
    return k.animations && k.openStyle != kOpenInstant ? float(k.openMs) / 1000.0f : 0.0f;
}

namespace {

// The info bar at the top right of the screen, like a classic menu's watermark: the title, then the FPS, the frame
// time, the resolution and the time of day on a dark see-through bar, all in the bold font, numbers in the accent
// color with small labels. Under every window.
void InfoBar(Ui& ui, const MenuState& s, const MenuFonts& f) {
    static float fps = 0.0f;  // smoothed over about a third of a second
    const float dt = ui.Input().deltaTime;
    if (dt > 0.0f) fps = fps > 0.0f ? fps + (1.0f / dt - fps) * std::min(1.0f, dt * 3.0f) : 1.0f / dt;
    char fpsText[16], msText[16], resolution[32], clock[32];
    std::snprintf(fpsText, sizeof(fpsText), "%d", int(fps + 0.5f));
    std::snprintf(msText, sizeof(msText), "%.1f", double(dt * 1000.0f));
    const Vec2 display = ui.GetDisplaySize();
    std::snprintf(resolution, sizeof(resolution), "%dx%d", int(display.x), int(display.y));
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    std::snprintf(clock, sizeof(clock), "%d:%02d:%02d %s", local.tm_hour % 12 ? local.tm_hour % 12 : 12, local.tm_min,
                  local.tm_sec, local.tm_hour < 12 ? "AM" : "PM");

    struct Piece {
        const char* text;
        Color color;
        bool small;  // a unit after a number
        float gap;   // before it
    };
    const Color text = ui.GetColor(UiColor::Text), accent = s.skin.accent, dim = ui.GetColor(UiColor::TextDisabled);
    const float gap = Px(11.0f), unitGap = Px(3.0f);
    Piece pieces[10];
    int count = 0;
    pieces[count++] = {"driz", text, false, 0.0f};
    pieces[count++] = {"menu", accent, false, 0.0f};
    if (s.showFps) {
        pieces[count++] = {fpsText, accent, false, gap};
        pieces[count++] = {"FPS", dim, true, unitGap};
    }
    if (s.showFrameTime) {
        pieces[count++] = {msText, accent, false, gap};
        pieces[count++] = {"MS", dim, true, unitGap};
    }
    pieces[count++] = {resolution, text, false, gap};
    pieces[count++] = {clock, text, false, gap};

    const Font& font = *f.bold;
    const float size = f.size, small = std::floor(f.size * 0.72f);
    float width = 0.0f;
    for (int i = 0; i < count; ++i) width += pieces[i].gap + std::ceil(font.MeasureText(pieces[i].text, pieces[i].small ? small : size).x);
    const float padX = Px(8.0f), lineH = std::floor(font.LineHeight() * size + 0.5f), barH = lineH + Px(6.0f);
    const float x1 = std::floor(display.x - Px(6.0f)), x0 = std::floor(x1 - width - padX * 2.0f), y0 = Px(6.0f);
    DrawList& dl = ui.BackgroundDrawList();
    dl.AddRectFilled(Rect(x0, y0, x1, y0 + barH), Rgba(10, 10, 10, 190));
    const float baseline = y0 + std::floor((barH - lineH) * 0.5f) + font.Ascent() * size;
    float x = x0 + padX;
    for (int i = 0; i < count; ++i) {
        const Piece& p = pieces[i];
        const float sz = p.small ? small : size;
        x += p.gap;
        dl.AddText(font, sz, {std::floor(x), std::floor(baseline - font.Ascent() * sz)}, p.color, p.text);
        x += std::ceil(font.MeasureText(p.text, sz).x);
    }
}

Vec2 g_menuRest;            // where the menu sits when it is not opening or closing
bool g_menuRestKnown = false;

} // namespace

void ShowDrizMenu(Ui& ui, TextureId carbon, bool* open, bool menuKey) {
    MenuState& s = State();
    MenuSkin& k = s.skin;
    // The menu key opens and closes it (not while a hotkey is waiting for its key, which may be this one).
    const bool binding = g_listening != nullptr;
    if (open && menuKey && !binding && ui.IsKeyPressed(kMenuKeys[std::clamp(s.menuKey, 0, 3)], false)) *open = !*open;
    const bool isOpen = !open || *open;
    // How far open (0..1, linear in time), and the eased amount the open/close style uses.
    const float progress = ui.Animate("##drizmenu_open", isOpen ? 1.0f : 0.0f, DrizMenuFadeTime());
    const float eased = 1.0f - (1.0f - progress) * (1.0f - progress);
    UpdateHotkeys(ui, s, menuKey || isOpen);

    const Vec2 display = ui.GetDisplaySize();
    const float resolutionScale = s.scaleWithResolution ? std::max(1.0f, display.y / 1080.0f) : 1.0f;
    const float previousScale = g_scale;
    g_scale = std::clamp(resolutionScale * float(s.menuScale) / 100.0f, 0.5f, 4.0f);
    const UiStyle saved = ui.Style();
    ApplySkin(ui.Style(), k);
    const MenuFonts fonts = SkinFonts(k, saved.font);
    g_fontSize = fonts.size;
    g_regularFont = fonts.regular;
    ui.Style().boldFont = fonts.bold;  // group box titles, slider values, dropdown rows
    ui.PushFont(fonts.regular, g_fontSize);
    if (s.watermark && (menuKey || progress > 0.0f)) InfoBar(ui, s, fonts);
    if (progress <= 0.0f) {
        ui.PopFont();
        ui.Style() = saved;
        return;
    }

    // Opening and closing: the chosen style, eased (Fade: the opacity alone).
    float fade = 1.0f, reveal = 1.0f;
    Vec2 offset;
    const float t = 1.0f - eased;
    switch (k.animations ? k.openStyle : int(kOpenInstant)) {
    case kOpenFade: fade = eased; break;
    case kOpenSlideDown: fade = eased; offset.y = -std::floor(Px(40.0f) * t); break;
    case kOpenSlideUp: fade = eased; offset.y = std::floor(Px(40.0f) * t); break;
    case kOpenSlideLeft: fade = eased; offset.x = -std::floor(Px(60.0f) * t); break;
    case kOpenSlideRight: fade = eased; offset.x = std::floor(Px(60.0f) * t); break;
    case kOpenUnroll: reveal = eased; break;
    default: break;
    }
    // A sliding menu is placed off its resting place by the offset for the whole animation (offset 0 included, so it
    // lands exactly where it was); its resting place is where it is while fully open (dragged there, say).
    const bool slides = k.openStyle >= kOpenSlideDown && k.openStyle <= kOpenSlideRight;
    if (g_menuRestKnown && slides && progress < 1.0f) {
        ui.SetNextWindowPos(g_menuRest + offset, Cond::Always);
    } else {
        ui.SetNextWindowPos({std::floor(display.x * 0.5f), std::floor(display.y * 0.5f)}, Cond::FirstUseEver, {0.5f, 0.5f});
    }
    if (reveal < 1.0f) ui.SetNextWindowReveal(reveal);
    // The classic 660 x 560 at 1080p, resized with the scale (after a resolution or Menu scale change too).
    ui.SetNextWindowSize({Px(660.0f), Px(560.0f)}, g_scale != previousScale ? Cond::Always : Cond::FirstUseEver);
    ui.PushStyleVar(UiStyleVar::Alpha, ui.Style().alpha * fade);
    ui.PushStyleVar(UiStyleVar::WindowPadding, Vec2(0.0f, 0.0f));
    const bool visible = ui.Begin("DrizMenu", nullptr, WindowFlags::NoTitleBar | WindowFlags::NoScrollbar);
    ui.PopStyleVar();
    if (progress >= 1.0f) {
        g_menuRest = ui.GetWindowPos();
        g_menuRestKnown = true;
    }
    // While it closes it is shown, not used: no input, drawn at full opacity.
    if (!isOpen) ui.Style().disabledAlpha = 1.0f;
    ui.BeginDisabled(!isOpen);
    if (visible) {
        // The background: the window's color, the carbon weave over it, and an animated effect over that, all under
        // the tab column and the panels (opaque unless Panel opacity says otherwise). Everything fades with the menu.
        DrawList& dl = ui.WindowDrawList();
        const float alpha = ui.GetAlpha();
        if (k.carbon && carbon != kNoTexture) {
            // One texel per pixel, tiled (the content area clips it): the weave stays crisp at any scale.
            const Vec2 pos = ui.GetWindowPos(), size = ui.GetWindowSize();
            const float tile = 256.0f;
            const Color tint = ScaleAlpha(colors::White, alpha);
            for (float y = pos.y; y < pos.y + size.y; y += tile) {
                for (float x = pos.x; x < pos.x + size.x; x += tile) {
                    dl.AddImage(carbon, Rect(x, y, x + tile, y + tile), {0.0f, 0.0f}, {1.0f, 1.0f}, tint);
                }
            }
        }
        if (k.effect != int(BackdropKind::None)) {
            Backdrop& b = k.backdrop;
            b.kind = BackdropKind(k.effect);
            b.speed = float(k.effectSpeed) / 100.0f;
            b.density = float(k.effectDensity) / 100.0f;
            b.opacity = float(k.effectOpacity) / 100.0f * alpha;
            b.scale = g_scale;
            BackdropColors effectColors;
            effectColors.primary = k.effectColor;
            effectColors.accent = k.accent;  // links to the mouse, rain heads, the sun
            b.Draw(dl, dl.ClipRect(), ui.Input().deltaTime, fonts.regular, effectColors,
                   ui.IsWindowHovered() ? ui.GetMousePos() : Vec2(-1e30f, -1e30f));
        }

        using IconFn = void (*)(DrawList&, Vec2, Color);
        using TabFn = void (*)(Ui&, MenuState&, const Font*);
        struct Tab {
            const char* id;
            IconFn icon;
            TabFn content;
        };
        static const Tab kTabs[] = {
            {"##player", IconPlayer, PlayerTab},     {"##world", IconWorld, WorldTab},
            {"##visuals", IconVisuals, VisualsTab},   {"##appearance", IconAppearance, AppearanceTab},
            {"##misc", IconMisc, MiscTab},           {"##customize", IconCustomize, CustomizeTab},
            {"##config", IconConfig, ConfigTab},
        };
        constexpr size_t kTabCount = sizeof(kTabs) / sizeof(kTabs[0]);
        struct Cell {
            Rect rect;
            bool selected;
            float hover;
        };
        Cell cells[kTabCount] = {};
        const float hoverTime = ui.Style().hoverTime;  // like the library's own tab labels
        if (ui.BeginTabBar("##tabs", TabBarFlags::Vertical, {Px(76.0f), Px(72.0f)})) {
            for (size_t i = 0; i < kTabCount; ++i) {
                // True for the selected tab, and for a moment after a switch also for the one sliding out.
                const bool show = ui.BeginTabItem(kTabs[i].id);
                cells[i] = {Rect(ui.GetItemRectMin(), ui.GetItemRectMax()), ui.IsTabItemSelected(),
                            ui.Animate(kTabs[i].id, ui.IsItemHovered() ? 1.0f : 0.0f, hoverTime)};
                if (show) {
                    kTabs[i].content(ui, s, fonts.bold);
                    ui.EndTabItem();
                }
            }
            ui.EndTabBar();
            // The icons go over the cells after the tab bar: while a switch animates, whatever is drawn between
            // BeginTabItem and EndTabItem fades and slides with the tab's contents (the icons would flicker).
            const Color on = ui.GetColor(UiColor::Text, 0.85f), off = ui.GetColor(UiColor::TextDisabled);
            for (size_t i = 0; i < kTabCount; ++i) {
                const Vec2 center(std::floor(cells[i].rect.Center().x) - 1.0f, std::floor(cells[i].rect.Center().y));
                kTabs[i].icon(dl, center, cells[i].selected ? on : Mix(off, on, cells[i].hover));
            }
        }
    }
    ui.EndDisabled();
    ui.End();
    ui.PopStyleVar();
    ui.PopFont();
    ui.Style() = saved;
}

} // namespace demo
