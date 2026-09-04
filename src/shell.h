// Custom window chrome: a hand-drawn title bar (drag to move, double-click to
// maximise, min/max/close buttons), a left icon navigation rail, a status bar,
// window-edge resize handles, and a full-window docking host.
#pragma once

#include "imgui.h"

struct GLFWwindow;

namespace shell {

inline constexpr float TITLEBAR_H = 30.0f;
inline constexpr float RAIL_W = 48.0f;
inline constexpr float STATUS_H = 26.0f;

// Menu-bar points shown in the title bar. Phase 3 makes them open real menus;
// for now they are inert hover targets so the layout is right.
extern const char* const MENU_POINTS[];
extern const int MENU_POINT_COUNT;

// One entry in the left icon rail.
struct NavItem {
    const char* icon;    // UTF-8 glyph, e.g. ICON_PHOTO_LIBRARY
    const char* tooltip;
};

// Configure the rail (pointer must stay valid). Call once at startup.
void set_nav(const NavItem* items, int count);
int  nav_active();
void set_nav_active(int index);

// Status-bar text (call each frame before begin()).
void set_status(const char* left, const char* right);

// Begin the frame: draw title bar + rail + status bar + open the docking host.
// Returns the dockspace id.
ImGuiID begin(GLFWwindow* window);

// End the docking host window.
void end();

// True only on the first frame (default layout just built).
bool first_run();

} // namespace shell
