// Shared sample code: a window that exercises every drizzy UI widget, with a Customize tab for themes, looks, fonts,
// colors and an animated backdrop. Used by the sandbox (gallery scene) and the overlay DLL (mod menu).
#pragma once

#include "demo_backdrops.h"
#include "demo_fonts.h"

#include "drizzy/ui.h"

#include <vector>

namespace demo {

// Everything the gallery's Customize tab and View menu change. The Ui's style holds the result; these remember the
// choices so they can be shown and combined (a theme and a look are independent).
struct GallerySettings {
    drizzy::UiTheme theme = drizzy::UiTheme::Dark;
    drizzy::UiLook look = drizzy::UiLook::Classic;
    std::vector<DemoFont> fonts;  // what the Font menu offers; set by the host (AddDemoFonts)
    int font = 0;                 // index into fonts
    // The quick colors: editing one rebuilds every UI color from them (UiStyle::ApplyPalette). Picking a theme resets
    // them to its palette.
    drizzy::UiPalette palette = drizzy::UiStyle::ThemePalette(drizzy::UiTheme::Dark);
    Backdrop backdrop;
    bool backdropThemeColors = true;  // backdrop colors follow the theme (text and accent) instead of `backdropColors`
    BackdropColors backdropColors;
};
GallerySettings& Gallery();

// Applies the settings' theme, look and font to `ui`'s style (call after changing them from code).
void ApplyGallerySettings(drizzy::Ui& ui);

// The widget gallery window. Pass an image texture to show Image / ImageButton; pass `open` to make it closable.
void ShowWidgetGallery(drizzy::Ui& ui, drizzy::TextureId image = drizzy::kNoTexture, bool* open = nullptr);

// Six small mod-menu windows side by side, each in its own look (and a theme and font that suit it), for comparing
// looks; or (`themes` = true) one window per theme in the current look. The Ui's style is restored afterwards.
void ShowStyleShowcase(drizzy::Ui& ui, bool themes = false);

} // namespace demo
