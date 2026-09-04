// Blockbench-styled widgets built on top of Dear ImGui.
// (Phase 4 fills this out; Phase 2 seeds it with panel chrome.)
#pragma once

#include "imgui.h"

namespace bb {

inline constexpr float PANEL_HEADER_H = 30.0f;
inline constexpr float ROW_H = 30.0f;

// Draw a Blockbench-style panel header (title row) as the first item inside a
// docked panel window. `title` is shown upper-cased in a small semibold face.
void panel_header(const char* title);

// Begin(name, NoTabBar host) + panel_header + a padded content region.
// Pair with end_panel(). Returns false if the window is collapsed/clipped
// (still call end_panel()).
bool begin_panel(const char* name);
void end_panel();

} // namespace bb
