// Shared sample code: a mod menu in the DrizMenu look and theme, laid out like a classic game menu: a column of icon
// tabs down the left, and settings in titled group boxes over a carbon-fiber background. Used by the sandbox
// (drizmenu scene) and, through the gallery's View menu, by the overlay DLL.
#pragma once

#include "drizzy/ui.h"

#include <cstdint>
#include <vector>

namespace demo {

// RGBA pixels of a size x size tile of the menu's carbon-fiber background (a 4x4 dither of two dark greys), drawn at
// one texel per pixel. Upload it as a texture and pass it to ShowDrizMenu.
std::vector<uint32_t> CarbonPixels(uint32_t size);

// The menu window. It switches the Ui to the DrizMenu look and theme (and Verdana, when the system has it) while it is
// built and restores the Ui's style afterwards, so it looks the same next to windows in any other style. `carbon` is
// the background texture (kNoTexture: a flat background).
void ShowDrizMenu(drizzy::Ui& ui, drizzy::TextureId carbon);

} // namespace demo
