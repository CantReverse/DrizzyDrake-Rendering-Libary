// Shared sample code: the fonts the samples let you switch between. Inter is the library's default font; the others
// are embedded into the samples at build time (all SIL Open Font License, see assets/fonts/*-LICENSE.txt).
#pragma once

#include "drizzy/font.h"

#include <vector>

namespace demo {

struct DemoFont {
    const char* name;
    drizzy::Font* font;
    float size;  // a comfortable UI size for it, in pixels per em (fonts differ a lot at the same em size)
};

// Adds the embedded demo fonts to `atlas` (call before atlas.Build()) and returns them after `defaultFont` (Inter,
// from FontAtlas::AddFontDefault) when it is given.
std::vector<DemoFont> AddDemoFonts(drizzy::FontAtlas& atlas, drizzy::Font* defaultFont);

} // namespace demo
