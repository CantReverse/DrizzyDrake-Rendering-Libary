// UI tables: fixed and stretching columns, resizing, sortable headers, row backgrounds, borders, and a scrolling body
// under a header that stays put.
//
// Cost: every column has its own draw list for the frame; a cell draws into its column's list, and EndTable appends
// the lists to the window's. Cells of one column share a clip rect, so the whole column becomes one draw command -
// a table costs about one draw call per column however many cells it has. Row backgrounds go to the window's list
// before the cells (under them), borders after (over them). Long tables use ListClipper, which only submits the
// visible rows; the table cooperates with it (rows end before the clipper measures, skipped rows keep row parity).
#include "ui_internal.h"

#include <algorithm>
#include <cmath>

namespace drizzy {
using namespace ui_detail;

namespace {

constexpr float kMinColumnWidth = 16.0f;
constexpr float kResizeHalfWidth = 4.0f;
constexpr ID kHeaderSeed = 0x48454144u;
constexpr ID kResizeSeed = 0x52455A45u;

float CellHeight(const UiState& g) { return TextLineHeight(g) + g.style.cellPadding.y * 2.0f; }

DrawList& ColumnList(UiState& g, const TableState& t, int column) {
    return *g.tableListPool[t.columnListBase + uint32_t(column)];
}

// Column x positions: fixed columns take their width, stretch columns share what is left by weight.
void ComputeColumnX(UiState& g, TableState& t) {
    float fixed = 0.0f, weights = 0.0f;
    for (TableColumn& c : t.columns) {
        if (c.flags & TableColumnFlags::WidthFixed) {
            if (c.width <= 0.0f) {
                // Fit the header: label, padding, and room for a sort arrow.
                c.width = std::max(TextSize(g, VisibleText(c.label)).x + g.style.cellPadding.x * 2.0f +
                                       ((t.flags & TableFlags::Sortable) ? g.fontSize : 0.0f),
                                   kMinColumnWidth * 2.0f);
            }
            fixed += c.width;
        } else {
            weights += c.weight;
        }
    }
    const float stretchSpace = std::max(t.x1 - t.x0 - fixed, 0.0f);
    float x = t.x0;
    for (TableColumn& c : t.columns) {
        const float w = (c.flags & TableColumnFlags::WidthFixed) ? c.width
                        : weights > 0.0f ? stretchSpace * c.weight / weights
                                         : 0.0f;
        c.x0 = std::floor(x);
        x += w;
        c.x1 = std::floor(x);
    }
    // Rounding must not leave a gap at the right edge.
    if (!t.columns.empty() && !(t.columns.back().flags & TableColumnFlags::WidthFixed)) t.columns.back().x1 = t.x1;
}

// Moves column i's right border to `border`; the columns left of it keep their widths. Without `trade` (a fixed column
// with only fixed columns before it) the column changes its width, and what follows moves or the stretch columns after
// it make room. With it the table's width pins everything right of the border, so the next column gives or takes the
// width.
void ResizeColumn(TableState& t, int i, float border, bool trade) {
    TableColumn& c = t.columns[size_t(i)];
    if (!trade) {
        c.width = std::max(border - c.x0, kMinColumnWidth);
        return;
    }
    TableColumn& next = t.columns[size_t(i + 1)];
    float fixed = 0.0f, weights = 0.0f;
    for (const TableColumn& col : t.columns) {
        if (col.flags & TableColumnFlags::WidthFixed) fixed += col.width;
        else weights += col.weight;
    }
    // Widths as ComputeColumnX lays them out, before rounding. A trade has a stretch column at or before i, so
    // weights > 0.
    const float space = std::max(t.x1 - t.x0 - fixed, 0.0f);
    auto widthOf = [&](const TableColumn& col) {
        return (col.flags & TableColumnFlags::WidthFixed) ? col.width : space * col.weight / weights;
    };
    const bool stretch = !(c.flags & TableColumnFlags::WidthFixed);
    const bool nextStretch = !(next.flags & TableColumnFlags::WidthFixed);
    const float width = widthOf(c), total = width + widthOf(next);
    if (total <= kMinColumnWidth * 2.0f) return;
    // From the rounded left edge, so the border settles on the mouse instead of flickering a pixel.
    const float newWidth = Clamp(border - c.x0, kMinColumnWidth, total - kMinColumnWidth);
    if (!stretch) c.width = newWidth;
    if (!nextStretch) next.width = total - newWidth;
    if (space <= 0.0f || (!stretch && !nextStretch)) return;
    // Stretch columns share the space by weight. Weights in proportion to the widths each should have now keep the
    // other stretch columns as they are, also when a fixed neighbour gave or took some of the space.
    const float moved = newWidth - width;
    const float newSpace = space + (stretch ? moved : 0.0f) - (nextStretch ? moved : 0.0f);
    for (size_t k = 0; k < t.columns.size(); ++k) {
        TableColumn& col = t.columns[k];
        if (col.flags & TableColumnFlags::WidthFixed) continue;
        const float target = k == size_t(i) ? newWidth : k == size_t(i + 1) ? total - newWidth : widthOf(col);
        col.weight = weights * target / newSpace;
    }
}

// Column border dragging. Runs before any cell is submitted so the borders win the mouse over cell widgets; it spans
// the table's height from last frame.
void HandleResize(UiState& g, TableState& t) {
    Window* w = t.window;
    const float top = t.hasHeader ? std::min(t.headerTop, t.top) : t.top;
    const float bottom = std::max(top + std::max(t.lastHeight, CellHeight(g)), top + CellHeight(g));
    const int n = int(t.columns.size());
    bool stretchSeen = false;
    for (int i = 0; i < n; ++i) {
        const TableColumn& c = t.columns[size_t(i)];
        // Once a column at or before the border stretches, the border can only move by trading width with the next
        // column, so it needs one that may be resized: the right-most border stays put while any column stretches.
        stretchSeen |= !(c.flags & TableColumnFlags::WidthFixed);
        const bool trade = stretchSeen;
        if (c.flags & TableColumnFlags::NoResize) continue;
        if (trade && (i == n - 1 || (t.columns[size_t(i + 1)].flags & TableColumnFlags::NoResize))) continue;
        const Rect hit(c.x1 - kResizeHalfWidth, top, c.x1 + kResizeHalfWidth, bottom);
        const ID id = HashData(&i, sizeof(i), t.id ^ kResizeSeed);
        bool hovered = false, held = false;
        const Rect clip = w->clipRect;
        w->clipRect = t.savedClip;
        ButtonBehavior(g, hit, id, &hovered, &held);
        w->clipRect = clip;
        if (hovered || held) g.cursorRequest = MouseCursor::ResizeEW;
        if (held) {
            // activeIdClickOffset is where in the handle it was grabbed: the border follows the mouse.
            ResizeColumn(t, i, g.mousePos.x - g.activeIdClickOffset.x + kResizeHalfWidth, trade);
            ComputeColumnX(g, t);
            t.resizing = i;
        } else if (hovered && t.resizing < 0) {
            t.resizing = i;  // highlighted, not dragged
        }
    }
}

// Once the setup calls are done (first header or row): column positions, resize handling, and the column lists.
void LockLayout(UiState& g, TableState& t) {
    if (t.layoutDone) return;
    t.layoutDone = true;
    for (TableColumn& c : t.columns) {
        if (!c.initialized) {
            c.initialized = true;  // a column without TableSetupColumn stretches evenly
            c.flags = TableColumnFlags::WidthStretch;
            c.weight = 1.0f;
        }
    }
    ComputeColumnX(g, t);
    if (t.flags & TableFlags::Resizable) HandleResize(g, t);
    for (int i = 0; i < int(t.columns.size()); ++i) {
        const TableColumn& c = t.columns[size_t(i)];
        DrawList& dl = ColumnList(g, t, i);
        dl.Reset(Rect(c.x0, t.bodyClip.min.y, c.x1, t.bodyClip.max.y).Intersect(t.bodyClip));
    }
}

void EndCell(UiState& g, TableState& t) {
    Window* w = t.window;
    // ItemSize keeps cursorMaxPos.y at the bottom of the cell's last line (it was set to the cell top on entry).
    const float contentBottom = w->cursorMaxPos.y;
    t.rowBottom = std::max(t.rowBottom, contentBottom + g.style.cellPadding.y);
    w->cursorMaxPos.y = std::max(t.cellMaxY, contentBottom);
}

bool BeginCell(UiState& g, TableState& t, int column) {
    Window* w = t.window;
    if (t.column >= 0) EndCell(g, t);
    t.column = column;
    const TableColumn& c = t.columns[size_t(column)];
    const Vec2 pad = g.style.cellPadding;
    // The cell becomes the window's layout area: its draw list, clip rect, content edges and default item width.
    w->dl = &ColumnList(g, t, column);
    w->clipRect = Rect(c.x0, t.bodyClip.min.y, c.x1, t.bodyClip.max.y).Intersect(t.bodyClip);
    w->contentStartX = c.x0 + pad.x;
    w->indent = 0.0f;
    w->cursorPos = {c.x0 + pad.x, t.rowTop + pad.y};
    w->cursorPosPrevLine = w->cursorPos;
    w->currLineHeight = w->prevLineHeight = 0.0f;
    w->currLineTextBaseOffset = w->prevLineTextBaseOffset = 0.0f;
    w->contentRegionMax.x = c.x1 - pad.x;
    w->itemWidth = std::max(1.0f, c.x1 - c.x0 - pad.x * 2.0f);
    t.cellMaxY = w->cursorMaxPos.y;
    w->cursorMaxPos.y = w->cursorPos.y;
    return c.x1 > t.bodyClip.min.x && c.x0 < t.bodyClip.max.x && t.rowTop < t.bodyClip.max.y &&
           t.rowTop + t.rowMinHeight > t.bodyClip.min.y;
}

} // namespace

namespace ui_detail {

TableState* CurrentTable(UiState& g) {
    if (g.tableStack.empty()) return nullptr;
    TableState* t = g.tableStack.back();
    return t->window == g.current ? t : nullptr;  // not from inside a child window begun in a cell
}

void TableEndRow(UiState& g, TableState& t) {
    if (!t.inRow) return;
    if (t.column >= 0) EndCell(g, t);
    const float bottom = std::floor(std::max(t.rowBottom, t.rowTop + t.rowMinHeight));
    // Backgrounds and row lines go to the window's list before the cells, so they sit under them. With a frozen
    // header they must not reach under it, so they start at the body's top.
    DrawList& main = *t.mainList;
    const float visibleTop = std::max(t.rowTop, t.bodyClip.min.y);
    if (!t.headerRow && (t.flags & TableFlags::RowBg) && bottom > visibleTop) {
        const Color bg = StyleColor(g, (t.row & 1) ? UiColor::TableRowBgAlt : UiColor::TableRowBg);
        if (ColorAlpha(bg)) main.AddRectFilled(Rect(t.x0, visibleTop, t.x1, bottom), bg);
    }
    if (!t.headerRow && (t.flags & TableFlags::BordersInnerH) && bottom - 1.0f >= t.bodyClip.min.y) {
        main.AddRectFilled(Rect(t.x0, bottom - 1.0f, t.x1, bottom), StyleColor(g, UiColor::TableBorder));
    }
    t.rowTop = bottom;
    t.inRow = false;
    t.column = -1;

    // Between rows the window lays out as itself again.
    Window* w = t.window;
    w->dl = t.mainList;
    w->clipRect = t.savedClip;
    w->contentStartX = t.savedContentStartX;
    w->indent = t.savedIndent;
    w->itemWidth = t.savedItemWidth;
    w->contentRegionMax = t.savedContentRegionMax;
    w->cursorPos = {t.x0, bottom};
    w->cursorPosPrevLine = w->cursorPos;
    w->currLineHeight = 0.0f;
    w->currLineTextBaseOffset = 0.0f;
    w->cursorMaxPos.y = std::max(w->cursorMaxPos.y, bottom);
}

void TableSeekRow(TableState& t, float y, int nextRow) {
    t.rowTop = y;
    t.row = nextRow - 1;
}

} // namespace ui_detail

// =====================================================================================================================
// Public API
// =====================================================================================================================
bool Ui::BeginTable(std::string_view strId, int columns, uint32_t flags, Vec2 outerSize) {
    UiState& g = *m;
    Window* w = g.current;
    DZ_ASSERT(w && "BeginTable must be inside a window");
    if (w->skipItems || columns <= 0) return false;
    const ID id = HashStr(strId, w->idStack.back());
    TableState& t = g.tables[id];
    if (int(t.columns.size()) != columns) {
        t.columns.assign(size_t(columns), TableColumn{});
        t.sort = {};
        t.sortInitialized = false;
        t.lastHeight = 0.0f;
    }
    t.id = id;
    t.flags = flags;

    const Vec2 avail = w->contentRegionMax - w->cursorPos;
    const float width = outerSize.x > 0.0f ? outerSize.x : std::max(avail.x + std::min(outerSize.x, 0.0f), 1.0f);
    if (flags & TableFlags::ScrollY) {
        // The rows scroll inside the table's own region; the header stays at its top.
        const float height = outerSize.y > 0.0f   ? outerSize.y
                             : outerSize.y < 0.0f ? std::max(avail.y + outerSize.y, CellHeight(g) * 3.0f)
                                                  : CellHeight(g) * 10.0f;
        char name[32];
        std::snprintf(name, sizeof(name), DZ_STR("##table_%08X"), id);
        if (!BeginChild(name, {width, height}, false)) {
            EndChild();
            return false;
        }
        w = g.current;
    }

    t.window = w;
    t.mainList = w->dl;
    t.savedDl = w->dl;
    t.savedClip = w->clipRect;
    t.savedContentStartX = w->contentStartX;
    t.savedIndent = w->indent;
    t.savedItemWidth = w->itemWidth;
    t.savedContentRegionMax = w->contentRegionMax;
    t.x0 = w->cursorPos.x;
    t.x1 = (flags & TableFlags::ScrollY) ? w->contentRegionMax.x : t.x0 + width;
    t.top = w->cursorPos.y;
    t.rowTop = t.top;
    t.rowBottom = t.top;
    t.row = -1;
    t.column = -1;
    t.inRow = t.headerRow = t.hasHeader = t.layoutDone = false;
    t.setupCount = 0;
    t.resizing = -1;
    t.bodyClip = t.savedClip;
    t.headerTop = t.headerBottom = t.top;

    t.columnListBase = g.tableListsUsed;
    g.tableListsUsed += uint32_t(columns);
    while (g.tableListPool.size() < g.tableListsUsed) g.tableListPool.push_back(std::make_unique<DrawList>());
    g.tableStack.push_back(&t);
    w->idStack.push_back(id);  // cell widgets get IDs of their own per table
    return true;
}

void Ui::TableSetupColumn(std::string_view label, uint32_t flags, float widthOrWeight) {
    UiState& g = *m;
    TableState* t = CurrentTable(g);
    DZ_ASSERT(t && !t->layoutDone && "TableSetupColumn goes right after BeginTable");
    if (!t || t->layoutDone || t->setupCount >= int(t->columns.size())) return;
    const int index = t->setupCount++;
    TableColumn& c = t->columns[size_t(index)];
    c.label.assign(label.data(), label.size());
    if (!(flags & (TableColumnFlags::WidthFixed | TableColumnFlags::WidthStretch))) flags |= TableColumnFlags::WidthStretch;
    const bool modeChanged = (c.flags & TableColumnFlags::WidthFixed) != (flags & TableColumnFlags::WidthFixed);
    if (!c.initialized || modeChanged) {
        // First frame (or the column changed kind): take the set-up size; later the user's resizing persists.
        c.initialized = true;
        c.width = (flags & TableColumnFlags::WidthFixed) ? std::max(widthOrWeight, 0.0f) : 0.0f;
        c.weight = (flags & TableColumnFlags::WidthFixed) ? 1.0f : (widthOrWeight > 0.0f ? widthOrWeight : 1.0f);
    }
    c.flags = flags;
    if (!t->sortInitialized && (t->flags & TableFlags::Sortable) && (flags & TableColumnFlags::DefaultSort)) {
        t->sort.column = index;
        t->sort.descending = (flags & TableColumnFlags::PreferSortDescending) != 0;
        t->sort.dirty = true;
    }
}

void Ui::TableHeadersRow() {
    UiState& g = *m;
    TableState* t = CurrentTable(g);
    DZ_ASSERT(t && "TableHeadersRow outside a table");
    if (!t) return;
    TableEndRow(g, *t);
    Window* w = t->window;
    const float h = CellHeight(g);
    if (t->flags & TableFlags::ScrollY) {
        // Frozen: drawn at the visible top of the scrolling region; its height is reserved at the top of the content
        // so the first row starts below it, and rows scrolled under it are clipped away.
        t->headerTop = t->savedClip.min.y;
        t->headerBottom = t->headerTop + h;
        t->bodyClip.min.y = std::max(t->bodyClip.min.y, t->headerBottom);
        t->rowTop += h;
    } else {
        t->headerTop = t->rowTop;
        t->headerBottom = t->rowTop + h;
        t->rowTop = t->headerBottom;
    }
    t->hasHeader = true;
    LockLayout(g, *t);

    const bool sortableTable = (t->flags & TableFlags::Sortable) != 0;
    const Vec2 pad = g.style.cellPadding;
    for (int i = 0; i < int(t->columns.size()); ++i) {
        const TableColumn& c = t->columns[size_t(i)];
        const Rect cell(c.x0, t->headerTop, c.x1, t->headerBottom);
        const Rect clip = cell.Intersect(t->savedClip);
        if (clip.Width() <= 0.0f || clip.Height() <= 0.0f) continue;
        DrawList& dl = ColumnList(g, *t, i);
        dl.PushClipRect(clip, false);
        w->dl = &dl;
        w->clipRect = clip;
        const bool sortable = sortableTable && !(c.flags & TableColumnFlags::NoSort);
        bool hovered = false, held = false;
        if (sortable) {
            const ID id = HashData(&i, sizeof(i), t->id ^ kHeaderSeed);
            if (ButtonBehavior(g, cell, id, &hovered, &held)) {
                if (t->sort.column == i) {
                    t->sort.descending = !t->sort.descending;
                } else {
                    t->sort.column = i;
                    t->sort.descending = (c.flags & TableColumnFlags::PreferSortDescending) != 0;
                }
                t->sort.dirty = true;
            }
        }
        const UiColor bg = held && hovered ? UiColor::HeaderActive : hovered ? UiColor::HeaderHovered
                                                                              : UiColor::TableHeaderBg;
        dl.AddRectFilled(cell, StyleColor(g, bg));
        RenderText(g, {c.x0 + pad.x, t->headerTop + pad.y}, VisibleText(c.label), StyleColor(g, UiColor::Text));
        if (sortable && t->sort.column == i) {
            RenderArrow(g, {c.x1 - pad.x - g.fontSize * 0.35f, (t->headerTop + t->headerBottom) * 0.5f},
                        g.fontSize * 0.45f, t->sort.descending ? 1 : 3, StyleColor(g, UiColor::Text));
        }
        dl.PopClipRect();
    }
    w->dl = t->mainList;
    w->clipRect = t->savedClip;
    w->cursorPos = {t->x0, t->rowTop};
    w->cursorMaxPos.y = std::max(w->cursorMaxPos.y, t->rowTop);
}

void Ui::TableNextRow(float minRowHeight) {
    UiState& g = *m;
    TableState* t = CurrentTable(g);
    DZ_ASSERT(t && "TableNextRow outside a table");
    if (!t) return;
    LockLayout(g, *t);
    TableEndRow(g, *t);
    t->row++;
    t->inRow = true;
    t->headerRow = false;
    t->column = -1;
    t->rowBottom = t->rowTop;
    t->rowMinHeight = std::max(minRowHeight, CellHeight(g));
}

bool Ui::TableNextColumn() {
    UiState& g = *m;
    TableState* t = CurrentTable(g);
    DZ_ASSERT(t && "TableNextColumn outside a table");
    if (!t) return false;
    if (!t->inRow || t->column + 1 >= int(t->columns.size())) TableNextRow();
    return BeginCell(g, *t, t->column + 1);
}

bool Ui::TableSetColumnIndex(int column) {
    UiState& g = *m;
    TableState* t = CurrentTable(g);
    if (!t || column < 0 || column >= int(t->columns.size())) return false;
    if (!t->inRow) TableNextRow();
    return BeginCell(g, *t, column);
}

int Ui::TableGetColumnIndex() const {
    TableState* t = CurrentTable(*m);
    return t ? t->column : -1;
}

int Ui::TableGetRowIndex() const {
    TableState* t = CurrentTable(*m);
    return t ? t->row : -1;
}

TableSortSpecs* Ui::TableGetSortSpecs() {
    TableState* t = CurrentTable(*m);
    return t && (t->flags & TableFlags::Sortable) ? &t->sort : nullptr;
}

void Ui::EndTable() {
    UiState& g = *m;
    DZ_ASSERT(!g.tableStack.empty() && "EndTable without BeginTable");
    if (g.tableStack.empty()) return;
    TableState& t = *g.tableStack.back();
    Window* w = t.window;
    DZ_ASSERT(w == g.current && "EndTable in another window than its BeginTable (a missing End/EndChild?)");
    LockLayout(g, t);  // a table without rows still lays out, and resets its lists
    TableEndRow(g, t);
    const float bottom = t.rowTop;

    // The cells, over the row backgrounds: one run per column.
    DrawList& main = *t.mainList;
    for (int i = 0; i < int(t.columns.size()); ++i) main.AppendList(ColumnList(g, t, i));

    // Borders over everything, across what is visible of the table.
    const bool frozen = t.hasHeader && (t.flags & TableFlags::ScrollY);
    const float top = frozen ? t.headerTop : t.top;
    const float visibleBottom = (t.flags & TableFlags::ScrollY) ? std::min(bottom, t.savedClip.max.y) : bottom;
    const Color border = StyleColor(g, UiColor::TableBorder);
    if (t.flags & TableFlags::BordersInnerV) {
        for (size_t i = 0; i + 1 < t.columns.size(); ++i) {
            const float x = t.columns[i].x1;
            main.AddRectFilled(Rect(x - 1.0f, top, x, visibleBottom), border);
        }
    }
    if (t.resizing >= 0) {
        const float x = t.columns[size_t(t.resizing)].x1;
        main.AddRectFilled(Rect(x - 1.0f, top, x + 1.0f, visibleBottom), StyleColor(g, UiColor::ResizeGripHovered));
    }
    if (t.hasHeader) main.AddRectFilled(Rect(t.x0, t.headerBottom - 1.0f, t.x1, t.headerBottom), border);
    if (t.flags & TableFlags::BordersOuter) main.AddRect(Rect(t.x0, top, t.x1, visibleBottom), border);

    t.lastHeight = bottom - t.top;
    t.sortInitialized = true;
    g.tableListsUsed = t.columnListBase;  // give the column lists back
    g.tableStack.pop_back();
    w->idStack.pop_back();

    if (t.flags & TableFlags::ScrollY) {
        w->cursorPos = {t.x0, bottom};
        w->cursorMaxPos = MaxV(w->cursorMaxPos, Vec2(t.x1, bottom));
        EndChild();
    } else {
        w->cursorPos = {t.x0, t.top};
        ItemSize(g, {t.x1 - t.x0, bottom - t.top});
        ItemAdd(g, Rect(t.x0, t.top, t.x1, bottom), 0);
    }
}

} // namespace drizzy
