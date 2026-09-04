// Font setup: Assistant (Blockbench's UI font) + merged Material Symbols icons.
#pragma once

#include "imgui.h"

namespace fonts {

// Load Assistant Regular + SemiBold from <exe dir>/assets/fonts, each with the
// icon font merged in. Call once after ImGui::CreateContext(), before the first
// frame. `dpi_scale` bakes the atlas at the right density.
void install(float dpi_scale);

// Assistant Regular (also the default font). Icons merged.
ImFont* body();
// Assistant SemiBold — menu points, headings. Icons merged.
ImFont* medium();

} // namespace fonts
