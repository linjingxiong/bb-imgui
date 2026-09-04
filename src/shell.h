// Custom window chrome: a hand-drawn title bar (drag to move, double-click to
// maximise, min/max/close buttons), window-edge resize handles, and a
// full-window docking host.
#pragma once

#include "imgui.h"

struct GLFWwindow;

namespace shell {

inline constexpr float TITLEBAR_H = 30.0f;

// Menu-bar points shown in the title bar. Phase 3 makes them open real menus;
// for now they are inert hover targets so the layout is right.
extern const char* const MENU_POINTS[];
extern const int MENU_POINT_COUNT;

// Begin the frame: draw the title bar + open the docking host window.
// Returns the dockspace id (for DockBuilder on first run).
ImGuiID begin(GLFWwindow* window);

// End the docking host window.
void end();

// True only on the first frame (host/dockspace just created) — build a default
// layout with DockBuilder when this is set.
bool first_run();

} // namespace shell
