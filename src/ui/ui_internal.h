// Internal: UI state and the helpers shared by the ui_*.cpp files. Not part of the public API.
#pragma once

#include "drizzy/ui.h"

#include <cstdarg>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace drizzy {
namespace ui_detail {

using ID = uint32_t;

ID HashData(const void* data, size_t size, ID seed);
ID HashStr(std::string_view str, ID seed);  // "###" restarts the hash so the ID ignores what precedes it
std::string_view VisibleText(std::string_view label);  // label up to "##"

// HashStr and VisibleText of a widget label in one scan.
struct Label {
    ID id;
    std::string_view text;
};
Label ParseLabel(std::string_view label, ID seed);

// Per-window widget state (tree nodes open, ...): a small sorted map, cheap for the handful of entries a window has.
class Storage {
public:
    bool Has(ID key) const;
    int GetInt(ID key, int defaultValue = 0) const;
    void SetInt(ID key, int value);
    float GetFloat(ID key, float defaultValue = 0.0f) const;
    void SetFloat(ID key, float value);

private:
    struct Pair {
        ID key;
        int value;
    };
    std::vector<Pair> m_pairs;
};

enum ItemStatus : uint32_t {
    kItemHoveredRect = 1u << 0,  // the mouse is inside the item's rect (ignores overlap / active items)
    kItemEdited = 1u << 1,
    kItemVisible = 1u << 2,
};

// Window flags reserved for the library, above the public WindowFlags bits.
constexpr uint32_t kWindowChild = 1u << 24;
constexpr uint32_t kWindowPopup = 1u << 25;
constexpr uint32_t kWindowModal = 1u << 26;
constexpr uint32_t kWindowTooltip = 1u << 27;
constexpr uint32_t kWindowCombo = 1u << 28;

struct GroupData {
    Vec2 cursorPos;
    Vec2 cursorMaxPos;
    float indent;
    float currLineHeight;
    float currLineTextBaseOffset;
};

struct Window {
    ID id = 0;
    std::string name;
    uint32_t flags = 0;
    Window* parent = nullptr;  // child windows: the window they are embedded in
    Window* root = nullptr;    // the top-level window whose draw list this window uses
    DrawList drawList;         // used by root windows only
    DrawList* dl = nullptr;

    Vec2 pos, size, sizeFull;  // sizeFull: size when not collapsed
    Vec2 contentSize;          // measured at the end of the last Begin/End
    Vec2 padding;
    Vec2 scroll, scrollMax;
    float scrollTargetY = -1.0f;
    float scrollTargetCenterRatio = 0.0f;
    float titleBarHeight = 0.0f;
    float menuBarHeight = 0.0f;  // WindowFlags::MenuBar: a strip below the title bar for the menu bar
    float borderSize = 0.0f;
    bool collapsed = false;
    bool active = false;     // Begin() was called this frame
    bool wasActive = false;  // Begin() was called last frame
    bool appearing = false;
    bool hidden = false;     // laid out but not drawn this frame (first frame of auto-sized windows)
    int hiddenFrames = 0;
    int autoFitFrames = 0;   // frames left during which the size follows the content
    int lastFrameActive = -1;
    int beginOrder = 0;
    bool scrollbarY = false;
    bool resizable = false;  // last Begin: the user can resize it (its hover area extends past the border)
    bool childBorder = false;
    bool skipItems = false;  // collapsed: widgets return early
    uint32_t setPosAllowed = 0xFFu, setSizeAllowed = 0xFFu, setCollapsedAllowed = 0xFFu;  // bit per Cond
    Vec2 pivotAnchor, pivot;  // SetNextWindowPos with a pivot, re-applied while the size settles
    int pivotFrames = 0;

    // Layout ("DC" in Dear ImGui)
    Vec2 cursorPos, cursorStartPos, cursorMaxPos, cursorPosPrevLine;
    float contentStartX = 0.0f;
    Vec2 contentRegionMax;  // bottom-right of the area items may use (screen space)
    float currLineHeight = 0.0f, prevLineHeight = 0.0f;
    float currLineTextBaseOffset = 0.0f, prevLineTextBaseOffset = 0.0f;
    float indent = 0.0f;
    float itemWidth = 0.0f;
    std::vector<float> itemWidthStack;
    std::vector<GroupData> groupStack;
    std::vector<ID> idStack;
    int treeDepth = 0;

    Rect outerRect;  // whole window
    Rect innerRect;  // inside borders, title bar, menu bar and scrollbar
    Rect menuBarRect;
    Rect clipRect;   // innerRect clipped to the parent

    // Menu bar layout, saved/restored around BeginMenuBar/EndMenuBar.
    bool inMenuBar = false;
    Vec2 menuBarSavedCursor, menuBarSavedCursorMax;
    Rect menuBarSavedClip;  // the content clip rect, replaced by the menu bar rect so bar items hover-test correctly

    // Last submitted item
    ID lastItemId = 0;
    Rect lastItemRect;
    uint32_t lastItemStatus = 0;

    Storage storage;
};

// Persisted window layout (SaveIniSettings / LoadIniSettings).
struct WindowSettings {
    std::string name;
    Vec2 pos, size;
    bool collapsed = false;
    bool hasPos = false, hasSize = false;
};

struct PopupData {
    ID popupId = 0;
    Window* window = nullptr;
    int openFrame = 0;
    Vec2 openMousePos;
    bool modal = false;
};

struct TabBarState {
    ID id = 0;
    ID selected = 0;
    ID nextSelected = 0;
    float rowY = 0.0f, rowHeight = 0.0f, startX = 0.0f, nextX = 0.0f, maxX = 0.0f;
    bool selectedSubmitted = false;
    Window* window = nullptr;
    // Tabs shrink to fit the bar when their natural widths add up to more: this frame's total, last frame's total.
    float naturalWidth = 0.0f, naturalWidthPrev = 0.0f;
};

struct TableColumn {
    std::string label;  // copied from TableSetupColumn: it is drawn later by TableHeadersRow
    uint32_t flags = 0;
    bool initialized = false;  // the persistent values below were set up
    float weight = 1.0f;       // stretch columns
    float width = 0.0f;        // fixed columns (set up or resized)
    float x0 = 0.0f, x1 = 0.0f;  // this frame, screen space
};

struct TableState {
    ID id = 0;
    uint32_t flags = 0;
    std::vector<TableColumn> columns;
    TableSortSpecs sort;
    bool sortInitialized = false;
    float lastHeight = 0.0f;   // last frame's height: resize handles span it
    int resizing = -1;         // column whose right border is being dragged

    // This frame.
    Window* window = nullptr;   // the window rows are laid out in (the table's own child with ScrollY)
    DrawList* mainList = nullptr;
    uint32_t columnListBase = 0;  // the columns' lists: UiState::tableListPool[columnListBase + column]
    int setupCount = 0;
    bool layoutDone = false;
    float x0 = 0.0f, x1 = 0.0f, top = 0.0f;
    float rowTop = 0.0f, rowBottom = 0.0f, rowMinHeight = 0.0f;
    int row = -1;       // body rows; -1 before the first
    int column = -1;    // current cell
    bool inRow = false;
    bool headerRow = false;
    bool hasHeader = false;
    float headerTop = 0.0f, headerBottom = 0.0f;
    Rect bodyClip;      // where rows are visible
    float cellMaxY = 0.0f;  // cursorMaxPos.y outside the current cell, restored when it ends
    // The window's own layout values, put back between cells and at EndTable.
    DrawList* savedDl = nullptr;
    Rect savedClip;
    float savedContentStartX = 0.0f, savedIndent = 0.0f, savedItemWidth = 0.0f;
    Vec2 savedContentRegionMax;
};

struct Notification {
    std::string text;
    NotifyType type = NotifyType::Info;
    float duration = 3.0f;
    float age = 0.0f;
};

struct InputTextState {
    ID id = 0;
    std::string text;         // edit buffer
    std::string initialText;  // restored by Escape
    int cursor = 0;    // caret, byte offset at a code point boundary
    int selStart = 0;  // selection anchor; the selection spans [min(selStart, cursor), max(selStart, cursor))
    float scrollX = 0.0f;
    float scrollY = 0.0f;       // multiline vertical scroll
    float preferredX = -1.0f;   // multiline: desired column (pixels) kept across Up/Down
    float blinkTime = 0.0f;
    bool selectAllOnMouseRelease = false;
    std::vector<int> boundaries;  // byte offset of each code point start, plus the end
    std::vector<float> offsets;   // pen x at each boundary
};

struct NextWindowData {
    bool hasPos = false, hasSize = false, hasCollapsed = false, hasFocus = false;
    Vec2 pos, pivot, size;
    Cond posCond = Cond::Always, sizeCond = Cond::Always, collapsedCond = Cond::Always;
    bool collapsed = false;
    bool hasAnchor = false;  // popups placed next to a widget flip above it when there is no room below
    Rect anchor;
    float maxHeight = 0.0f;
    bool childBorder = false;
    void Clear() { *this = NextWindowData(); }
};

struct NextItemData {
    bool hasWidth = false;
    float width = 0.0f;
    bool hasOpen = false;
    bool open = false;
    Cond openCond = Cond::Always;
};

struct ColorMod {
    UiColor color;
    Color backup;
};

struct StyleMod {
    UiStyleVar var;
    float backup[2];
};

struct FontMod {
    const Font* font;
    float size;
};

struct VarInfo {
    const char* name;
    uint8_t count;   // 1 = float, 2 = Vec2
    size_t offset;   // into UiStyle
};
const VarInfo& GetVarInfo(UiStyleVar var);

constexpr int kMouseButtons = 3;
constexpr int kKeyCount = int(Key::Count);

} // namespace ui_detail

struct UiState {
    using Window = ui_detail::Window;
    using ID = ui_detail::ID;

    UiStyle style;
    UiInput input;
    UiOutput output;
    UiPlatform platform;
    int frameCount = 0;
    double time = 0.0;
    bool withinFrame = false;

    // Copies of the current input, for internal use and the GetDisplaySize / GetMousePos queries.
    Vec2 displaySize;
    Vec2 mousePos = {-1e30f, -1e30f};

    // Derived input
    Vec2 mousePosPrev = {-1e30f, -1e30f};
    Vec2 mouseDelta;
    bool mouseDownPrev[ui_detail::kMouseButtons] = {};
    bool mouseClicked[ui_detail::kMouseButtons] = {};
    bool mouseReleased[ui_detail::kMouseButtons] = {};
    bool mouseDoubleClicked[ui_detail::kMouseButtons] = {};
    double mouseClickedTime[ui_detail::kMouseButtons] = {-1e9, -1e9, -1e9};
    Vec2 mouseClickedPos[ui_detail::kMouseButtons];
    float keysDownDuration[ui_detail::kKeyCount] = {};
    float keysDownDurationPrev[ui_detail::kKeyCount] = {};

    // Windows
    std::vector<std::unique_ptr<Window>> windows;
    std::unordered_map<ID, Window*> windowsById;
    std::vector<Window*> focusOrder;   // top-level, non-popup windows, back to front
    std::vector<Window*> windowStack;  // Begin/End nesting
    Window* current = nullptr;
    Window* hoveredWindow = nullptr;       // top-level window under the mouse
    Window* hoveredWindowInner = nullptr;  // innermost (child) window under the mouse
    Window* movingWindow = nullptr;
    Window* focusedWindow = nullptr;
    uint32_t resizeEdges = 0;  // while a window is being resized: the edges being dragged (ResizeEdge bits)
    Rect resizeStartRect;      // its position and size when the drag started
    Vec2 resizeStartMouse;
    int beginOrderCounter = 0;
    int rootWindowsCreated = 0;

    // Items
    ID hoveredId = 0;
    ID hoveredIdPrev = 0;
    ID activeId = 0;
    ID activeIdPrevFrame = 0;
    Window* activeIdWindow = nullptr;
    bool activeIdAlive = false;
    bool activeIdJustActivated = false;
    Vec2 activeIdClickOffset;
    float dragAccumulator = 0.0f;

    // Popups
    std::vector<ui_detail::PopupData> openPopups;       // stack of open popups / modals
    std::vector<ui_detail::PopupData> beginPopupStack;  // popups whose content is being submitted

    std::unordered_map<ID, ui_detail::TabBarState> tabBars;
    std::vector<ui_detail::TabBarState*> tabBarStack;
    std::unordered_map<ID, ui_detail::TableState> tables;
    std::vector<ui_detail::TableState*> tableStack;
    // Column draw lists for the tables being built; tables take a run of them and give it back at EndTable.
    std::vector<std::unique_ptr<DrawList>> tableListPool;
    uint32_t tableListsUsed = 0;
    std::vector<ui_detail::Notification> notifications;

    ui_detail::NextWindowData nextWindow;
    ui_detail::NextItemData nextItem;
    std::vector<ui_detail::ColorMod> colorStack;
    std::vector<ui_detail::StyleMod> styleStack;
    std::vector<ui_detail::FontMod> fontStack;
    std::vector<float> disabledStack;  // alpha to restore
    const Font* font = nullptr;
    float fontSize = 16.0f;
    float alpha = 1.0f;
    int disabled = 0;

    ui_detail::InputTextState inputText;
    std::unordered_map<ID, ui_detail::WindowSettings> windowSettings;
    // Color picker hue/sat kept across frames (RGB loses hue when saturation or value hits 0).
    ID colorPickerId = 0;
    float colorPickerHue = 0.0f, colorPickerSat = 0.0f, colorPickerVal = 0.0f;
    ID popupClosedByClick = 0;  // lets a combo's button close its popup instead of reopening it
    MouseCursor cursorRequest = MouseCursor::Arrow;
    int tooltipDepth = 0;

    PrimAllocator* primAllocator = nullptr;  // given to every draw list (SetPrimAllocator)
    DrawList background;
    DrawList foreground;
    std::vector<const DrawList*> renderLists;
    DrawData drawData;

    char formatBuffer[2048] = {};
    std::string scratchText;
    std::vector<Vec2> scratchPoints;
};

namespace ui_detail {

// ---- Frame-level helpers (ui_core.cpp) -------------------------------------------------------------------------------
Window* FindWindow(UiState& g, ID id);
void FocusWindow(UiState& g, Window* window);
void SetActiveID(UiState& g, ID id, Window* window);
void ClearActiveID(UiState& g);
void KeepAliveID(UiState& g, ID id);
bool IsPopupOpenId(const UiState& g, ID id);
void OpenPopupEx(UiState& g, ID id);
bool BeginPopupEx(Ui& ui, UiState& g, ID id, uint32_t flags);
void ClosePopupToLevel(UiState& g, size_t level);
ID MoveId(const Window* window);
bool IsMouseValid(Vec2 pos);

// ---- Layout and interaction ------------------------------------------------------------------------------------------
float TextLineHeight(const UiState& g);
float FrameHeight(const UiState& g);
Vec2 TextSize(const UiState& g, std::string_view text, float wrapWidth = 0.0f);
void ItemSize(UiState& g, Vec2 size, float textBaseOffset = 0.0f);
bool ItemAdd(UiState& g, const Rect& bb, ID id);  // false when clipped: skip drawing
bool ItemHoverable(UiState& g, const Rect& bb, ID id);
enum ButtonFlags : uint32_t {
    kButtonPressOnClick = 1u << 0,        // pressed on mouse down instead of release
    kButtonMouseRight = 1u << 1,          // react to the right button
    kButtonAllowWhenOverlapped = 1u << 2,
    kButtonAlignTextBaseLine = 1u << 3,   // shift down to the line's text baseline (buttons without vertical padding)
};
bool ButtonBehavior(UiState& g, const Rect& bb, ID id, bool* hovered, bool* held, uint32_t flags = 0);
float CalcItemWidth(const UiState& g);
Vec2 CalcItemSize(const UiState& g, Vec2 size, float defaultW, float defaultH);
bool IsKeyPressedImpl(const UiState& g, Key key, bool repeat);
bool IsMouseHovering(const UiState& g, const Rect& r);

// ---- Rendering helpers -----------------------------------------------------------------------------------------------
Color StyleColor(const UiState& g, UiColor color, float alphaScale = 1.0f);
void RenderFrame(UiState& g, const Rect& r, Color fill, bool border, float rounding);
void RenderText(UiState& g, Vec2 pos, std::string_view text, Color color, float wrapWidth = 0.0f);
// Text clipped to `clip` by trimming its glyphs (DrawList::AddTextClipped): unlike PushClipRect, no extra draw command.
void RenderTextClipped(UiState& g, Vec2 pos, std::string_view text, Color color, const Rect& clip);
// Text inside `r`, aligned by `align` (0..1 per axis) and clipped to `clip`. Pass `size` when the caller already
// measured the text (TextSize), so it is not measured twice.
void RenderTextAligned(UiState& g, const Rect& r, std::string_view text, Vec2 align, const Rect* clip = nullptr,
                       const Vec2* size = nullptr);
void RenderArrow(UiState& g, Vec2 center, float size, int dir, Color color);  // dir: 0 right, 1 down, 2 left, 3 up
void RenderCheckMark(UiState& g, Vec2 pos, Color color, float size);

// ---- Tables and notifications (ui_tables.cpp, ui_extras.cpp) ----------------------------------------------------------
TableState* CurrentTable(UiState& g);
void TableEndRow(UiState& g, TableState& t);  // closes the open row, if any
void TableSeekRow(TableState& t, float y, int nextRow);  // ListClipper skipped rows: continue at y with row nextRow
void RenderNotifications(UiState& g);

const char* FormatV(UiState& g, const char* format, va_list args);
const char* Format(UiState& g, const char* format, ...);
// One number through a printf-style format. The usual widget formats ("%d", "%i", "%f", "%.3f", with literal text
// around them and "%%") are converted with std::to_chars; anything else goes through vsnprintf.
const char* FormatNumber(UiState& g, const char* format, double value);
const char* FormatNumber(UiState& g, const char* format, int value);

} // namespace ui_detail
} // namespace drizzy
