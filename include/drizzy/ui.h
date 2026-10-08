// drizzy_renderer - immediate-mode UI.
//
// Dear ImGui-style: widgets are function calls that draw and report interaction in the same call, every frame, with
// no widget objects to create, store or destroy. The UI never touches the OS. Each frame the host fills UiInput from
// its own input system, reads UiOutput back (does the UI want the mouse / keyboard, which cursor to show), and renders
// the DrawData returned by Render() with a backend.
//
//   drizzy::Ui ui;
//   ui.Style().font = font;
//   ...each frame:
//   ui.Input().displaySize = {width, height};
//   ui.Input().mousePos = ...;  ui.Input().mouseDown[0] = ...;  ui.Input().AddCharacter(...);
//   ui.NewFrame();
//   if (ui.Begin("Settings")) {
//       ui.SliderFloat("Volume", &volume, 0.0f, 1.0f);
//       if (ui.Button("Apply")) Apply();
//   }
//   ui.End();
//   renderer.Render(ui.Render());
//   if (!ui.Output().wantCaptureMouse) GameHandlesMouse();
//
// Conventions follow Dear ImGui where it costs nothing: labels are also IDs ("Play##menu" shows "Play", "###id" keeps
// an ID stable while the visible text changes), labels sit to the right of framed widgets, and Begin / BeginChild /
// BeginPopup... return whether their contents are visible.
#pragma once

#include "drizzy/draw_list.h"
#include "drizzy/font.h"

#include <cfloat>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(_MSC_VER)
#include <sal.h>
#define DZ_FORMAT_STRING _Printf_format_string_
#define DZ_FORMAT_ATTR(fmt, args)
#else
#define DZ_FORMAT_STRING
#define DZ_FORMAT_ATTR(fmt, args) __attribute__((format(printf, fmt, args)))
#endif

namespace drizzy {

struct UiState;

// Keys the UI reacts to. Hosts map their own key codes onto these.
enum class Key : uint8_t {
    Tab, LeftArrow, RightArrow, UpArrow, DownArrow, PageUp, PageDown, Home, End, Insert, Delete, Backspace, Space,
    Enter, Escape, A, C, V, X, Y, Z,
    Count
};

enum class MouseButton : uint8_t { Left, Right, Middle };

enum class MouseCursor : uint8_t { Arrow, TextInput, Hand, ResizeAll, ResizeNS, ResizeEW, ResizeNWSE, ResizeNESW };

// When a SetNext...() call applies.
enum class Cond : uint8_t {
    Always,        // every frame
    Once,          // once per run
    FirstUseEver,  // only for a window that has never existed (no size/position yet)
    Appearing,     // when the window becomes visible after being hidden
};

struct WindowFlags {
    enum : uint32_t {
        None = 0,
        NoTitleBar = 1u << 0,
        NoResize = 1u << 1,
        NoMove = 1u << 2,
        NoScrollbar = 1u << 3,
        NoScrollWithMouse = 1u << 4,
        NoCollapse = 1u << 5,
        AlwaysAutoResize = 1u << 6,   // size the window to its content every frame
        NoBackground = 1u << 7,
        NoInputs = 1u << 8,           // mouse passes through to whatever is behind
        NoBringToFrontOnFocus = 1u << 9,
        NoShadow = 1u << 10,
        MenuBar = 1u << 11,           // reserve a menu bar row at the top (fill it with BeginMenuBar)
        NoSavedSettings = 1u << 12,   // do not persist this window's position/size/collapsed state
        NoDecoration = NoTitleBar | NoResize | NoScrollbar | NoCollapse,
    };
};

struct InputTextFlags {
    enum : uint32_t {
        None = 0,
        CharsDecimal = 1u << 0,      // 0-9 . + - e E
        CharsNoBlank = 1u << 1,      // no spaces or tabs
        CharsUppercase = 1u << 2,
        AutoSelectAll = 1u << 3,     // select everything when the field is activated
        EnterReturnsTrue = 1u << 4,  // return true on Enter instead of on every edit
        Password = 1u << 5,          // display '*' instead of the text, disable copy
        ReadOnly = 1u << 6,
        Multiline = 1u << 7,         // used internally by InputTextMultiline
    };
};

struct ColorEditFlags {
    enum : uint32_t {
        None = 0,
        NoAlpha = 1u << 0,    // ignore the alpha channel (RGB only)
        NoInputs = 1u << 1,   // no RGB/hex text boxes, just the swatch / picker
        NoPicker = 1u << 2,   // clicking the swatch does not open a picker popup
        NoLabel = 1u << 3,    // do not draw the label to the right
        DisplayHex = 1u << 4, // the text box shows "#RRGGBBAA" instead of R/G/B/A integers
    };
};

struct TreeNodeFlags {
    enum : uint32_t {
        None = 0,
        DefaultOpen = 1u << 0,
        Leaf = 1u << 1,  // no arrow, never opens (still clickable)
    };
};

struct SelectableFlags {
    enum : uint32_t {
        None = 0,
        DontClosePopups = 1u << 0,  // clicking does not close the popup the selectable is in
        Disabled = 1u << 1,
        SpanAllColumns = 1u << 2,   // in a table: the highlight and the click area cover the whole row
    };
};

struct TableFlags {
    enum : uint32_t {
        None = 0,
        Resizable = 1u << 0,      // drag the borders between columns to resize them
        Sortable = 1u << 1,       // click a header to sort by it (read TableGetSortSpecs)
        RowBg = 1u << 2,          // alternating row backgrounds
        BordersInnerV = 1u << 3,  // lines between columns
        BordersInnerH = 1u << 4,  // lines between rows
        BordersOuter = 1u << 5,   // a frame around the table
        Borders = BordersInnerV | BordersInnerH | BordersOuter,
        ScrollY = 1u << 6,        // fixed height (outerSize.y); the rows scroll under a header that stays put
    };
};

struct TableColumnFlags {
    enum : uint32_t {
        None = 0,
        WidthFixed = 1u << 0,            // a width in UI units (widthOrWeight; 0 fits the header)
        WidthStretch = 1u << 1,          // a share of the remaining width (widthOrWeight is the weight; the default)
        NoResize = 1u << 2,
        NoSort = 1u << 3,
        DefaultSort = 1u << 4,           // sorted by this column until the user picks another
        PreferSortDescending = 1u << 5,  // first click sorts descending
    };
};

// What the user asked to sort by (TableFlags::Sortable). Sort your rows when `dirty`, then clear it.
struct TableSortSpecs {
    int column = -1;  // -1: not sorted
    bool descending = false;
    bool dirty = false;
};

enum class NotifyType : uint8_t { Info, Success, Warning, Error };

enum class UiColor : uint8_t {
    Text, TextDisabled, WindowBg, ChildBg, PopupBg, Border, WindowShadow, FrameBg, FrameBgHovered, FrameBgActive,
    TitleBg, TitleBgActive, ScrollbarBg, ScrollbarGrab, ScrollbarGrabHovered, ScrollbarGrabActive, CheckMark,
    SliderGrab, SliderGrabActive, Button, ButtonHovered, ButtonActive, Header, HeaderHovered, HeaderActive, Separator,
    ResizeGrip, ResizeGripHovered, ResizeGripActive, Tab, TabHovered, TabActive, PlotLines, PlotHistogram,
    TextSelectedBg, ModalDimBg, MenuBarBg, TableHeaderBg, TableBorder, TableRowBg, TableRowBgAlt,
    Count
};

// Style values that PushStyleVar can override temporarily.
enum class UiStyleVar : uint8_t {
    Alpha, WindowPadding, WindowRounding, WindowBorderSize, WindowShadowSize, WindowMinSize, FramePadding,
    FrameRounding, FrameBorderSize, ItemSpacing, ItemInnerSpacing, IndentSpacing, ScrollbarSize, ScrollbarRounding,
    GrabMinSize, GrabRounding, PopupRounding, TabRounding, ButtonTextAlign, CellPadding,
    Count
};

struct UiStyle {
    const Font* font = nullptr;   // required
    float fontSize = 16.0f;       // pixels per em
    float alpha = 1.0f;           // global opacity
    Vec2 windowPadding = {10.0f, 10.0f};
    float windowRounding = 8.0f;
    float windowBorderSize = 1.0f;
    float windowShadowSize = 18.0f;  // soft shadow behind top-level windows; 0 disables
    Vec2 windowMinSize = {64.0f, 40.0f};
    Vec2 framePadding = {8.0f, 5.0f};
    float frameRounding = 5.0f;
    float frameBorderSize = 0.0f;
    Vec2 itemSpacing = {8.0f, 6.0f};
    Vec2 itemInnerSpacing = {6.0f, 5.0f};
    float indentSpacing = 20.0f;
    float scrollbarSize = 12.0f;
    float scrollbarRounding = 6.0f;
    float grabMinSize = 12.0f;
    float grabRounding = 4.0f;
    float popupRounding = 6.0f;
    float tabRounding = 5.0f;
    Vec2 buttonTextAlign = {0.5f, 0.5f};
    Vec2 cellPadding = {6.0f, 3.0f};  // inside table cells
    float disabledAlpha = 0.5f;
    Color colors[size_t(UiColor::Count)] = {};

    UiStyle();  // dark theme
    static UiStyle Dark();
    static UiStyle Light();

    // Multiplies every size by `scale` (for DPI scaling). Does not touch fontSize.
    void ScaleAllSizes(float scale);

    // Themes as text: one "name = value" per line, '#' starts a comment.
    //   windowRounding = 8          framePadding = 8, 5          color.Button = #3A3F55FF
    // Unknown names and malformed values are reported in `error` (with line numbers) and skipped.
    bool LoadTheme(std::string_view text, std::string* error = nullptr);
    std::string SaveTheme() const;

    static const char* ColorName(UiColor color);
    static const char* VarName(UiStyleVar var);
};

// Input for one frame, filled by the host before Ui::NewFrame().
struct UiInput {
    Vec2 displaySize;                     // pixels; top-level windows are kept inside it
    float deltaTime = 1.0f / 60.0f;       // seconds since the last frame
    Vec2 mousePos = {-1e30f, -1e30f};     // pixels; leave far off-screen when there is no mouse
    bool mouseDown[3] = {};               // indexed by MouseButton
    float mouseWheel = 0.0f;              // vertical wheel notches since the last frame, + scrolls up; reset by Render
    float mouseWheelH = 0.0f;             // horizontal wheel notches; reset by Render
    bool keysDown[size_t(Key::Count)] = {};
    bool keyCtrl = false, keyShift = false, keyAlt = false;
    std::vector<uint32_t> characters;     // text typed since the last frame (code points); cleared by Render

    void AddCharacter(uint32_t codepoint);
    void AddCharactersUtf8(std::string_view text);
    void SetKey(Key key, bool down) { keysDown[size_t(key)] = down; }
};

// What the host should do after a frame.
struct UiOutput {
    bool wantCaptureMouse = false;     // the mouse is over the UI or dragging a widget: keep it from the game
    bool wantCaptureKeyboard = false;  // a widget has keyboard focus: keep keys from the game
    bool wantTextInput = false;        // a text field is active (show an on-screen keyboard / enable IME)
    Vec2 textInputPos;                 // caret position in display pixels, for IME candidate windows
    MouseCursor cursor = MouseCursor::Arrow;
};

// Optional OS services the host can provide. The library never calls the OS itself.
struct UiPlatform {
    void* userData = nullptr;
    const char* (*getClipboardText)(void* userData) = nullptr;
    void (*setClipboardText)(void* userData, const char* text) = nullptr;
};

class Ui {
public:
    Ui();
    ~Ui();
    Ui(const Ui&) = delete;
    Ui& operator=(const Ui&) = delete;

    // ---- Frame ------------------------------------------------------------------------------------------------------
    UiStyle& Style();
    UiInput& Input();
    UiPlatform& Platform();
    const UiOutput& Output() const;
    void NewFrame();
    // Ends the frame. The returned data (lists ordered back to front) stays valid until the next NewFrame.
    const DrawData& Render();
    uint32_t FrameCount() const;
    double Time() const;

    // The render target size fed in through UiInput::displaySize. For high-DPI displays, scale the style up with
    // UiStyle::ScaleAllSizes and use a larger font size.
    Vec2 GetDisplaySize() const;

    // ---- Settings: window layout persistence ------------------------------------------------------------------------
    // Saves each window's position, size and collapsed state to a small text blob; load it back next run so menus
    // reopen where the user left them. Windows with WindowFlags::NoSavedSettings are skipped. Load before the first
    // NewFrame (or any time; it applies as each window next appears).
    std::string SaveIniSettings() const;
    void LoadIniSettings(std::string_view data);

    // ---- Custom drawing ---------------------------------------------------------------------------------------------
    DrawList& WindowDrawList();      // current window, clipped to its content area
    DrawList& BackgroundDrawList();  // behind every window (HUDs, crosshairs)
    DrawList& ForegroundDrawList();  // above every window

    // ---- Windows ----------------------------------------------------------------------------------------------------
    // Always call End(), whatever Begin returns. Passing `open` adds a close button that sets *open = false.
    bool Begin(std::string_view name, bool* open = nullptr, uint32_t flags = WindowFlags::None);
    void End();
    // A scrollable region inside the current window. Size components <= 0 mean "remaining space minus that much".
    // Always call EndChild().
    bool BeginChild(std::string_view id, Vec2 size = {}, bool border = false, uint32_t flags = WindowFlags::None);
    void EndChild();
    // Pivot (0..1) picks which point of the window lands on `pos`: {0.5, 0.5} centers it.
    void SetNextWindowPos(Vec2 pos, Cond cond = Cond::Always, Vec2 pivot = {});
    // A component of 0 sizes that axis to the content.
    void SetNextWindowSize(Vec2 size, Cond cond = Cond::Always);
    void SetNextWindowCollapsed(bool collapsed, Cond cond = Cond::Always);
    void SetNextWindowFocus();
    Vec2 GetWindowPos() const;
    Vec2 GetWindowSize() const;
    bool IsWindowHovered() const;   // the current window (or one of its children) is under the mouse
    bool IsWindowFocused() const;
    bool IsWindowAppearing() const;
    float GetScrollY() const;
    float GetScrollMaxY() const;
    void SetScrollY(float scrollY);
    void SetScrollHereY(float centerRatio = 0.5f);  // scroll so the cursor position is at that fraction of the view

    // ---- Layout -----------------------------------------------------------------------------------------------------
    void SameLine(float offsetFromStartX = 0.0f, float spacing = -1.0f);
    void NewLine();
    void Spacing();
    void Separator();
    void Dummy(Vec2 size);
    void Indent(float width = 0.0f);
    void Unindent(float width = 0.0f);
    void BeginGroup();  // lay out several items, then treat them as one for SameLine / IsItemHovered
    void EndGroup();
    void PushItemWidth(float width);  // > 0 pixels, < 0 "available width minus that much"
    void PopItemWidth();
    void SetNextItemWidth(float width);
    float CalcItemWidth() const;
    void AlignTextToFramePadding();  // vertically align following text with framed widgets on the same line
    Vec2 GetContentRegionAvail() const;
    Vec2 GetCursorPos() const;  // relative to the window
    void SetCursorPos(Vec2 localPos);
    Vec2 GetCursorScreenPos() const;
    void SetCursorScreenPos(Vec2 pos);
    float GetTextLineHeight() const;
    float GetFrameHeight() const;
    float GetFrameHeightWithSpacing() const;
    Vec2 CalcTextSize(std::string_view text, float wrapWidth = 0.0f) const;

    // ---- IDs --------------------------------------------------------------------------------------------------------
    void PushID(std::string_view id);
    void PushID(int id);
    void PushID(const void* id);
    void PopID();
    uint32_t GetID(std::string_view id) const;

    // ---- Style overrides --------------------------------------------------------------------------------------------
    void PushStyleColor(UiColor color, Color value);
    void PopStyleColor(int count = 1);
    void PushStyleVar(UiStyleVar var, float value);
    void PushStyleVar(UiStyleVar var, Vec2 value);
    void PopStyleVar(int count = 1);
    void PushFont(const Font* font, float size);
    void PopFont();
    void BeginDisabled(bool disabled = true);  // greys out and blocks interaction
    void EndDisabled();
    Color GetColor(UiColor color, float alphaScale = 1.0f) const;  // style color with current alpha applied

    // ---- Text -------------------------------------------------------------------------------------------------------
    void Text(std::string_view text);
    void TextF(DZ_FORMAT_STRING const char* format, ...) DZ_FORMAT_ATTR(2, 3);
    void TextColored(Color color, std::string_view text);
    void TextDisabled(std::string_view text);
    void TextWrapped(std::string_view text);
    void LabelText(std::string_view label, std::string_view value);
    void BulletText(std::string_view text);
    void SeparatorText(std::string_view label);
    bool TextLink(std::string_view label);  // clickable text in the accent color; true when clicked
    void HelpMarker(std::string_view text);  // a "(?)" that shows `text` as a tooltip when hovered

    // ---- Buttons and toggles ----------------------------------------------------------------------------------------
    // Size components: 0 = fit the label, < 0 = available space minus that much.
    bool Button(std::string_view label, Vec2 size = {});
    bool SmallButton(std::string_view label);
    bool InvisibleButton(std::string_view id, Vec2 size);
    bool ImageButton(std::string_view id, TextureId texture, Vec2 size, Vec2 uv0 = {0.0f, 0.0f},
                     Vec2 uv1 = {1.0f, 1.0f}, Color tint = colors::White);
    void Image(TextureId texture, Vec2 size, Vec2 uv0 = {0.0f, 0.0f}, Vec2 uv1 = {1.0f, 1.0f},
               Color tint = colors::White, float rounding = 0.0f);
    bool Checkbox(std::string_view label, bool* value);
    bool RadioButton(std::string_view label, bool active);
    bool RadioButton(std::string_view label, int* value, int buttonValue);
    // An on/off switch (the knob slides over); true when toggled.
    bool ToggleSwitch(std::string_view label, bool* value);
    // A negative fraction shows an indeterminate (busy) bar.
    void ProgressBar(float fraction, Vec2 size = {-1.0f, 0.0f}, std::string_view overlay = {});
    // A spinning arc for work in progress. radius 0: half the font size.
    void Spinner(std::string_view id, float radius = 0.0f, float thickness = 2.5f);
    void Bullet();

    // ---- Sliders and drags ------------------------------------------------------------------------------------------
    bool SliderFloat(std::string_view label, float* value, float min, float max, const char* format = "%.3f");
    bool SliderInt(std::string_view label, int* value, int min, int max, const char* format = "%d");
    // Drag horizontally to change the value (Shift = 10x, Alt = 0.1x). min >= max means unclamped.
    bool DragFloat(std::string_view label, float* value, float speed = 1.0f, float min = 0.0f, float max = 0.0f,
                   const char* format = "%.3f");
    bool DragInt(std::string_view label, int* value, float speed = 1.0f, int min = 0, int max = 0,
                 const char* format = "%d");
    // `count` components side by side in one item width (positions, colors, ranges). True when any changed.
    bool SliderFloatN(std::string_view label, float* values, int count, float min, float max,
                      const char* format = "%.3f");
    bool SliderIntN(std::string_view label, int* values, int count, int min, int max, const char* format = "%d");
    bool DragFloatN(std::string_view label, float* values, int count, float speed = 1.0f, float min = 0.0f,
                    float max = 0.0f, const char* format = "%.3f");
    bool DragIntN(std::string_view label, int* values, int count, float speed = 1.0f, int min = 0, int max = 0,
                  const char* format = "%d");
    bool SliderFloat2(std::string_view l, float v[2], float mn, float mx, const char* f = "%.3f") { return SliderFloatN(l, v, 2, mn, mx, f); }
    bool SliderFloat3(std::string_view l, float v[3], float mn, float mx, const char* f = "%.3f") { return SliderFloatN(l, v, 3, mn, mx, f); }
    bool SliderFloat4(std::string_view l, float v[4], float mn, float mx, const char* f = "%.3f") { return SliderFloatN(l, v, 4, mn, mx, f); }
    bool SliderInt2(std::string_view l, int v[2], int mn, int mx, const char* f = "%d") { return SliderIntN(l, v, 2, mn, mx, f); }
    bool SliderInt3(std::string_view l, int v[3], int mn, int mx, const char* f = "%d") { return SliderIntN(l, v, 3, mn, mx, f); }
    bool SliderInt4(std::string_view l, int v[4], int mn, int mx, const char* f = "%d") { return SliderIntN(l, v, 4, mn, mx, f); }
    bool DragFloat2(std::string_view l, float v[2], float s = 1.0f, float mn = 0.0f, float mx = 0.0f, const char* f = "%.3f") { return DragFloatN(l, v, 2, s, mn, mx, f); }
    bool DragFloat3(std::string_view l, float v[3], float s = 1.0f, float mn = 0.0f, float mx = 0.0f, const char* f = "%.3f") { return DragFloatN(l, v, 3, s, mn, mx, f); }
    bool DragFloat4(std::string_view l, float v[4], float s = 1.0f, float mn = 0.0f, float mx = 0.0f, const char* f = "%.3f") { return DragFloatN(l, v, 4, s, mn, mx, f); }
    bool DragInt2(std::string_view l, int v[2], float s = 1.0f, int mn = 0, int mx = 0, const char* f = "%d") { return DragIntN(l, v, 2, s, mn, mx, f); }
    bool DragInt3(std::string_view l, int v[3], float s = 1.0f, int mn = 0, int mx = 0, const char* f = "%d") { return DragIntN(l, v, 3, s, mn, mx, f); }
    bool DragInt4(std::string_view l, int v[4], float s = 1.0f, int mn = 0, int mx = 0, const char* f = "%d") { return DragIntN(l, v, 4, s, mn, mx, f); }
    // An angle stored in radians, edited in degrees.
    bool SliderAngle(std::string_view label, float* radians, float minDegrees = -360.0f, float maxDegrees = 360.0f,
                     const char* format = "%.0f deg");

    // ---- Text input -------------------------------------------------------------------------------------------------
    // Edits a NUL-terminated buffer in place (no allocations). Returns true when the text changed (or, with
    // InputTextFlags::EnterReturnsTrue, when Enter was pressed).
    bool InputText(std::string_view label, char* buffer, size_t bufferSize, uint32_t flags = InputTextFlags::None);
    bool InputText(std::string_view label, std::string* text, uint32_t flags = InputTextFlags::None);
    bool InputTextWithHint(std::string_view label, std::string_view hint, char* buffer, size_t bufferSize,
                           uint32_t flags = InputTextFlags::None);
    bool InputInt(std::string_view label, int* value, uint32_t flags = InputTextFlags::None);
    bool InputFloat(std::string_view label, float* value, const char* format = "%.3f",
                    uint32_t flags = InputTextFlags::None);
    // Multi-line editor. `size` as usual (0 = default height); scrolls and wraps. Supports Enter for newlines.
    bool InputTextMultiline(std::string_view label, char* buffer, size_t bufferSize, Vec2 size = {},
                            uint32_t flags = InputTextFlags::None);
    bool InputTextMultiline(std::string_view label, std::string* text, Vec2 size = {},
                            uint32_t flags = InputTextFlags::None);

    // ---- Color ------------------------------------------------------------------------------------------------------
    // A color swatch. Returns true when clicked. `size` of 0 fits one frame height square.
    bool ColorButton(std::string_view id, Color color, Vec2 size = {}, uint32_t flags = ColorEditFlags::None);
    // Swatch + optional R/G/B(/A) or hex box + label; clicking the swatch opens a picker. Edits `color` in place.
    bool ColorEdit3(std::string_view label, Color* color, uint32_t flags = ColorEditFlags::None);
    bool ColorEdit4(std::string_view label, Color* color, uint32_t flags = ColorEditFlags::None);
    // The full picker: saturation/value square, hue bar, and (unless NoAlpha) an alpha bar, plus inputs.
    bool ColorPicker4(std::string_view label, Color* color, uint32_t flags = ColorEditFlags::None);

    // ---- Combos, lists, menus ---------------------------------------------------------------------------------------
    bool BeginCombo(std::string_view label, std::string_view preview, int maxVisibleItems = 8);
    void EndCombo();  // only if BeginCombo returned true
    bool Combo(std::string_view label, int* current, const char* const items[], int count, int maxVisibleItems = 8);
    bool Selectable(std::string_view label, bool selected = false, uint32_t flags = SelectableFlags::None,
                    Vec2 size = {});
    bool Selectable(std::string_view label, bool* selected, uint32_t flags = SelectableFlags::None, Vec2 size = {});
    bool BeginListBox(std::string_view label, Vec2 size = {});
    void EndListBox();  // only if BeginListBox returned true
    bool MenuItem(std::string_view label, std::string_view shortcut = {}, bool selected = false, bool enabled = true);

    // ---- Menu bars --------------------------------------------------------------------------------------------------
    // A menu bar along the top of the current window; the window needs WindowFlags::MenuBar. EndMenuBar() only if true.
    bool BeginMenuBar();
    void EndMenuBar();
    // A menu bar across the top of the screen. EndMainMenuBar() only if it returned true.
    bool BeginMainMenuBar();
    void EndMainMenuBar();
    // A menu (drop-down from a bar, or a submenu inside another menu). EndMenu() only if it returned true.
    bool BeginMenu(std::string_view label, bool enabled = true);
    void EndMenu();

    // ---- Trees ------------------------------------------------------------------------------------------------------
    bool TreeNode(std::string_view label, uint32_t flags = TreeNodeFlags::None);  // TreePop() if it returned true
    void TreePop();
    bool CollapsingHeader(std::string_view label, uint32_t flags = TreeNodeFlags::None);
    void SetNextItemOpen(bool open, Cond cond = Cond::Always);

    // ---- Tabs -------------------------------------------------------------------------------------------------------
    bool BeginTabBar(std::string_view id);  // EndTabBar() if it returned true
    void EndTabBar();
    bool BeginTabItem(std::string_view label);  // EndTabItem() if it returned true; true for the selected tab
    void EndTabItem();

    // ---- Popups, modals, tooltips -----------------------------------------------------------------------------------
    void OpenPopup(std::string_view id);
    bool BeginPopup(std::string_view id, uint32_t flags = WindowFlags::None);  // EndPopup() if it returned true
    // Blocks interaction with everything behind it and dims the screen. Centered unless positioned.
    bool BeginPopupModal(std::string_view name, bool* open = nullptr, uint32_t flags = WindowFlags::None);
    // Opens on right-click over the last item.
    bool BeginPopupContextItem(std::string_view id = {});
    void EndPopup();
    void CloseCurrentPopup();
    bool IsPopupOpen(std::string_view id) const;
    void SetTooltip(std::string_view text);
    void BeginTooltip();
    void EndTooltip();

    // ---- Plots ------------------------------------------------------------------------------------------------------
    void PlotLines(std::string_view label, const float* values, int count, int offset = 0,
                   std::string_view overlay = {}, float scaleMin = FLT_MAX, float scaleMax = FLT_MAX,
                   Vec2 size = {});
    void PlotHistogram(std::string_view label, const float* values, int count, int offset = 0,
                       std::string_view overlay = {}, float scaleMin = FLT_MAX, float scaleMax = FLT_MAX,
                       Vec2 size = {});

    // ---- Tables -----------------------------------------------------------------------------------------------------
    // Rows of cells in aligned columns: fixed or stretching columns, resizing, sorting by header, row backgrounds,
    // borders, and a scrolling body under a header that stays put. Each column draws into its own list, merged at
    // EndTable, so a table costs about one draw call per column, not one per cell. For long tables, wrap the rows in a
    // ListClipper: only the visible rows are submitted.
    //
    //   if (ui.BeginTable("items", 3, TableFlags::RowBg | TableFlags::Borders | TableFlags::ScrollY, {0, 300})) {
    //       ui.TableSetupColumn("Name");  ui.TableSetupColumn("Count", TableColumnFlags::WidthFixed, 80);
    //       ui.TableSetupColumn("Price");
    //       ui.TableHeadersRow();
    //       ListClipper clipper;  clipper.Begin(ui, int(items.size()));
    //       while (clipper.Step())
    //           for (int i = clipper.displayStart; i < clipper.displayEnd; ++i) {
    //               ui.TableNextRow();
    //               ui.TableNextColumn(); ui.Text(items[i].name);
    //               ui.TableNextColumn(); ui.TextF("%d", items[i].count);
    //               ui.TableNextColumn(); ui.TextF("%.2f", items[i].price);
    //           }
    //       ui.EndTable();
    //   }
    // outerSize: 0 = available width / grow with the rows (needs a height with ScrollY); < 0 = available minus that.
    bool BeginTable(std::string_view id, int columns, uint32_t flags = TableFlags::None, Vec2 outerSize = {});
    void EndTable();  // only if BeginTable returned true
    // Before the first row. Columns not set up stretch evenly.
    void TableSetupColumn(std::string_view label, uint32_t flags = TableColumnFlags::None, float widthOrWeight = 0.0f);
    void TableHeadersRow();  // a row of clickable headers (sorting, resizing) from the column labels
    void TableNextRow(float minRowHeight = 0.0f);
    bool TableNextColumn();  // moves to the next cell (starting a row when needed); false when it is scrolled out
    bool TableSetColumnIndex(int column);
    int TableGetColumnIndex() const;
    int TableGetRowIndex() const;  // body rows from 0
    TableSortSpecs* TableGetSortSpecs();  // null unless TableFlags::Sortable

    // ---- Notifications ----------------------------------------------------------------------------------------------
    // A toast in the bottom-right corner that fades out after `seconds` (hovering keeps it). Stacks with others.
    void Notify(std::string_view text, NotifyType type = NotifyType::Info, float seconds = 3.0f);

    // ---- Item and input queries -------------------------------------------------------------------------------------
    bool IsItemHovered() const;
    bool IsItemActive() const;
    bool IsItemClicked(MouseButton button = MouseButton::Left) const;
    bool IsItemEdited() const;          // the last item changed its value this frame
    bool IsItemActivated() const;       // the last item became active this frame
    bool IsItemDeactivated() const;     // the last item stopped being active this frame
    Vec2 GetItemRectMin() const;
    Vec2 GetItemRectMax() const;
    Vec2 GetItemRectSize() const;
    bool IsAnyItemHovered() const;
    bool IsAnyItemActive() const;
    bool IsMouseHoveringRect(const Rect& rect) const;
    bool IsKeyDown(Key key) const;
    bool IsKeyPressed(Key key, bool repeat = true) const;
    bool IsMouseDown(MouseButton button) const;
    bool IsMouseClicked(MouseButton button) const;
    bool IsMouseReleased(MouseButton button) const;
    bool IsMouseDoubleClicked(MouseButton button) const;
    Vec2 GetMousePos() const;
    Vec2 GetMouseDragDelta(MouseButton button = MouseButton::Left) const;
    void SetMouseCursor(MouseCursor cursor);

    // ---- Rendering --------------------------------------------------------------------------------------------------
    // Records every draw list of the UI straight into the backend's GPU memory (Renderer implements PrimAllocator),
    // so rendering copies nothing. Render the DrawData of each frame with that renderer, before the next NewFrame.
    void SetPrimAllocator(PrimAllocator* allocator);

private:
    friend class ListClipper;
    std::unique_ptr<UiState> m;
};

// Lays out only the visible rows of a long list of equally tall rows; the others cost nothing, so a list of 100000
// rows costs what its visible rows do. The rows' height is measured from the first row unless given.
//
//   ListClipper clipper;
//   clipper.Begin(ui, int(items.size()));
//   while (clipper.Step())
//       for (int i = clipper.displayStart; i < clipper.displayEnd; ++i) ui.Text(items[i]);
class ListClipper {
public:
    ListClipper() = default;
    ~ListClipper() { End(); }
    ListClipper(const ListClipper&) = delete;
    ListClipper& operator=(const ListClipper&) = delete;

    // `itemHeight`: distance between the tops of two rows (row height plus item spacing); <= 0 measures it.
    void Begin(Ui& ui, int itemCount, float itemHeight = -1.0f);
    // Sets displayStart / displayEnd to the next range of rows to submit; false when the list is done.
    bool Step();
    // Moves the layout past every row. Step() does this when it returns false; call it to stop early.
    void End();

    int displayStart = 0;
    int displayEnd = 0;

private:
    Ui* m_ui = nullptr;
    int m_count = 0;
    int m_step = 0;
    float m_itemHeight = 0.0f;
    float m_startY = 0.0f;
};

// A search box and the matching it drives, for filtering long lists and tables. Comma-separated terms; a row passes
// when it contains any plain term and none of the terms starting with '-' ("sword,axe,-rusty"). Case-insensitive.
// Terms are parsed when the text changes, so PassFilter is a few substring searches.
//
//   static TextFilter filter;
//   filter.Draw(ui, "Search");
//   for (const Item& item : items) if (filter.PassFilter(item.name)) ui.Text(item.name);
class TextFilter {
public:
    bool Draw(Ui& ui, std::string_view label = "Filter", float width = 0.0f);  // true when the text changed
    bool PassFilter(std::string_view text) const;
    bool IsActive() const { return m_termCount > 0; }
    void Clear();
    void Set(std::string_view text);

private:
    void Parse();
    struct Term {
        uint16_t offset = 0, length = 0;
        bool exclude = false;
    };
    static constexpr int kMaxTerms = 16;
    char m_text[128] = {};
    char m_lower[128] = {};  // lowercase copy the terms point into
    Term m_terms[kMaxTerms];
    int m_termCount = 0;
    bool m_hasInclude = false;
};

} // namespace drizzy
