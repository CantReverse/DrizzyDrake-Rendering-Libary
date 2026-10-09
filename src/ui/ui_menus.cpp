// Menu bars and menus: BeginMenuBar / BeginMainMenuBar / BeginMenu, built on the popup stack so bar menus and
// submenus share one code path (they differ only in layout and where the drop-down opens).
#include "ui_internal.h"

#include <algorithm>
#include <cstdio>

namespace drizzy {
using namespace ui_detail;

bool Ui::BeginMenuBar() {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems || !(w->flags & WindowFlags::MenuBar) || w->menuBarRect.Height() <= 0.0f) return false;
    DZ_ASSERT(!w->inMenuBar && "BeginMenuBar already active");
    w->menuBarSavedCursor = w->cursorPos;
    w->menuBarSavedCursorMax = w->cursorMaxPos;
    w->menuBarSavedClip = w->clipRect;
    w->clipRect = w->menuBarRect;  // so bar items hover-test against the bar strip, not the content area
    w->dl->PushClipRect(w->menuBarRect, false);
    w->cursorPos = {w->menuBarRect.min.x + g.style.itemInnerSpacing.x, w->menuBarRect.min.y};
    w->cursorMaxPos = w->cursorPos;
    w->inMenuBar = true;
    return true;
}

void Ui::EndMenuBar() {
    UiState& g = *m;
    Window* w = g.current;
    DZ_ASSERT(w->inMenuBar && "EndMenuBar without BeginMenuBar");
    w->dl->PopClipRect();
    w->cursorPos = w->menuBarSavedCursor;
    w->cursorMaxPos = w->menuBarSavedCursorMax;
    w->clipRect = w->menuBarSavedClip;
    w->inMenuBar = false;
}

bool Ui::BeginMainMenuBar() {
    UiState& g = *m;
    SetNextWindowPos({0.0f, 0.0f});
    SetNextWindowSize({g.displaySize.x, 0.0f});
    PushStyleVar(UiStyleVar::WindowRounding, 0.0f);
    PushStyleVar(UiStyleVar::WindowBorderSize, 0.0f);
    PushStyleVar(UiStyleVar::WindowPadding, Vec2(g.style.windowPadding.x, 0.0f));
    const uint32_t flags = WindowFlags::NoTitleBar | WindowFlags::NoResize | WindowFlags::NoMove |
                           WindowFlags::NoScrollbar | WindowFlags::NoCollapse | WindowFlags::NoSavedSettings |
                           WindowFlags::NoBringToFrontOnFocus | WindowFlags::NoShadow | WindowFlags::MenuBar |
                           WindowFlags::AlwaysAutoResize;
    bool open = Begin(DZ_STR("##MainMenuBar"), nullptr, flags);
    PopStyleVar(3);
    if (open) open = BeginMenuBar();
    if (!open) End();
    return open;
}

void Ui::EndMainMenuBar() {
    EndMenuBar();
    End();
}

bool Ui::BeginMenu(std::string_view label, bool enabled) {
    UiState& g = *m;
    Window* w = g.current;
    if (w->skipItems) return false;
    const auto [id, text] = ParseLabel(label, w->idStack.back());
    const bool inBar = w->inMenuBar;
    const UiStyle& style = g.style;

    const size_t level = g.beginPopupStack.size();
    bool anyOpenAtLevel = g.openPopups.size() > level;
    bool menuOpen = anyOpenAtLevel && g.openPopups[level].popupId == id;

    const float labelW = TextSize(g, text).x;
    const Vec2 startPos = w->cursorPos;
    Rect bb;
    if (inBar) {
        bb = Rect(startPos.x, w->menuBarRect.min.y, startPos.x + labelW + style.framePadding.x * 2.0f,
                  w->menuBarRect.max.y);
    } else {
        // Natural width (label + submenu arrow) drives the popup's auto-fit; the row stretches to the window width.
        const float h = TextLineHeight(g) + style.framePadding.y * 2.0f;
        const float naturalW = style.framePadding.x * 2.0f + labelW + g.fontSize + style.itemInnerSpacing.x;
        ItemSize(g, {naturalW, h}, style.framePadding.y);
        bb = Rect(startPos.x, startPos.y, std::max(w->contentRegionMax.x, startPos.x + naturalW), startPos.y + h);
    }

    bool hovered = false, held = false, pressed = false;
    if (inBar) {
        w->lastItemId = id;
        w->lastItemRect = bb;
        w->lastItemStatus = bb.Overlaps(w->clipRect) ? kItemVisible : 0;
        if (IsMouseHovering(g, bb)) w->lastItemStatus |= kItemHoveredRect;
        KeepAliveID(g, id);
        pressed = ButtonBehavior(g, bb, id, &hovered, &held, kButtonPressOnClick);
    } else {
        if (ItemAdd(g, bb, id)) pressed = ButtonBehavior(g, bb, id, &hovered, &held);
    }
    if (!enabled) hovered = pressed = false;

    bool wantOpen = false, wantClose = false;
    if (enabled) {
        if (inBar) {
            if (pressed) (menuOpen ? wantClose : wantOpen) = true;
            else if (anyOpenAtLevel && !menuOpen && hovered) wantOpen = true;  // slide across the bar
        } else {
            if (hovered && !menuOpen) wantOpen = true;  // submenus open on hover
        }
    }
    if (wantOpen) {
        OpenPopupEx(g, id);
        menuOpen = true;
    } else if (wantClose) {
        ClosePopupToLevel(g, level);
        menuOpen = false;
    }

    // Draw the label.
    DrawList& dl = *w->dl;
    const Color textColor = StyleColor(g, enabled ? UiColor::Text : UiColor::TextDisabled);
    if (hovered || menuOpen) {
        const UiColor c = menuOpen ? UiColor::HeaderActive : UiColor::HeaderHovered;
        dl.AddRectFilled(bb, StyleColor(g, c), inBar ? style.frameRounding : 0.0f);
    }
    if (inBar) {
        RenderTextAligned(g, bb, text, {0.5f, 0.5f}, &bb);
        w->cursorPos.x = bb.max.x + 2.0f;
        w->cursorMaxPos.x = std::max(w->cursorMaxPos.x, w->cursorPos.x);
    } else {
        RenderText(g, {bb.min.x + style.framePadding.x, bb.min.y + style.framePadding.y}, text, textColor);
        RenderArrow(g, {bb.max.x - g.fontSize * 0.7f, bb.Center().y}, g.fontSize * 0.5f, 0, textColor);
    }

    if (!menuOpen) return false;

    // Open the drop-down: below a bar item, to the right of a submenu row.
    g.nextWindow.hasPos = true;
    g.nextWindow.posCond = Cond::Always;
    g.nextWindow.pivot = {};
    g.nextWindow.hasAnchor = true;
    g.nextWindow.anchor = bb;
    if (inBar) {
        g.nextWindow.pos = {bb.min.x, bb.max.y};
    } else {
        g.nextWindow.pos = {bb.max.x, bb.min.y - style.windowPadding.y};
    }
    const bool visible = BeginPopupEx(*this, g, id, WindowFlags::AlwaysAutoResize | WindowFlags::NoMove);
    return visible;
}

void Ui::EndMenu() { EndPopup(); }

} // namespace drizzy
