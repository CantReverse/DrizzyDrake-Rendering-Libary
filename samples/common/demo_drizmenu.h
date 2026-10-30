// Shared sample code: a mod menu in the DrizMenu look and theme, laid out like a classic game menu: a column of icon
// tabs down the left, and settings in titled group boxes over a carbon-fiber background with drifting particles. Its
// Customize tab restyles the menu itself: colors, font (Minecraft-style pixel text included), widget shapes, sizes,
// animation, background effect and panel opacity. Used by the sandbox (drizmenu scene) and, through the gallery's View
// menu, by the overlay DLL.
#pragma once

#include "drizzy/ui.h"

#include <cstdint>
#include <vector>

namespace demo {

// RGBA pixels of a size x size tile of the menu's carbon-fiber weave (a 4x4 pattern of clear and third-black texels,
// laid over the background color), drawn at one texel per pixel. Upload it as a texture and pass it to ShowDrizMenu.
std::vector<uint32_t> CarbonPixels(uint32_t size);

// The menu window, and the info bar at the top right of the screen (Misc > Info bar). Call it every frame. It switches
// the Ui to its own style (the DrizMenu look and theme as customized, in Verdana when the system has it) while it is
// built and restores the Ui's style afterwards, so it looks the same next to windows in any other style. `carbon` is
// the weave texture (kNoTexture: a flat background). Its font list is Gallery().fonts.
// With `open`, the menu opens and closes as *open changes, animated (Customize > Animation > Open and close); with
// `menuKey` it also toggles *open itself when its key (Misc > Menu key, Insert by default) is pressed, its hotkeys work
// while it is closed, and the info bar stays up. Without `open` it is always open.
void ShowDrizMenu(drizzy::Ui& ui, drizzy::TextureId carbon, bool* open = nullptr, bool menuKey = false);

// How long the menu takes to open and close (Customize > Animation > Open and close time, 85 ms by default; 0 with
// animations off), for a host that fades more than the menu with its own toggle.
float DrizMenuFadeTime();

} // namespace demo
