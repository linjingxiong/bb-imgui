// Custom window chrome: a hand-drawn title bar (drag to move, double-click to
// maximise, min/max/close buttons), a project tab bar, a toolbar with a
// mode selector (Edit/Paint/Animate), a status bar, window-edge resize
// handles, and a full-window docking host. Structure mirrors Blockbench's
// own window.css (#tab_bar, #mode_selector, .tool) — measurements pulled
// from the real stylesheet, not guessed.
#pragma once

#include "imgui.h"
#include "menu.h"

struct GLFWwindow;

namespace shell {

inline constexpr float TITLEBAR_H = 26.0f; // Blockbench #title_bar height
inline constexpr float TAB_BAR_H = 34.0f;  // Blockbench #tab_bar height
inline constexpr float TOOLBAR_H = 30.0f;  // Blockbench .tool height
inline constexpr float STATUS_H = 26.0f;

// Title-bar menu points (File / Edit / …). Pointer must stay valid.
void set_menus(const menu::Menu* menus, int count);

// Label of the menu item activated this frame, or nullptr. Valid after begin().
const char* menu_clicked();

// One open project tab (Blockbench: #tab_bar .project_tab — one per open
// model file). `modified` draws the unsaved-changes dot instead of a close X
// until hovered, matching Blockbench.
struct ProjectTab {
    const char* name;
    bool modified;
};
void set_tabs(const ProjectTab* tabs, int count);
int  tabs_active();
void set_tabs_active(int index);

// Blockbench's three editing modes (#mode_selector), shown right-aligned in
// the toolbar row.
enum class Mode { Edit, Paint, Animate };
void set_mode(Mode m);
Mode mode();

// One tool icon button drawn left-aligned in the toolbar row, before the
// mode selector. Call between toolbar_begin()/toolbar_end().
bool tool_button(const char* icon_glyph, bool active = false);
void toolbar_begin(const char* panel_label);
void toolbar_end();

// Status-bar text (call each frame before begin()).
void set_status(const char* left, const char* right);
// A small bordered pill drawn at the far right of the status bar (real
// Blockbench: a "Collections" tab). Optional — pass "" for none.
void set_status_tab(const char* label);

// Begin the frame: draw title bar + tab bar + status bar + open the docking
// host (the toolbar row is drawn separately via toolbar_begin/tool_button*/
// toolbar_end so callers can add their own tool icons per mode).
ImGuiID begin(GLFWwindow* window);

// End the docking host window.
void end();

// True only on the first frame (default layout just built).
bool first_run();

} // namespace shell
