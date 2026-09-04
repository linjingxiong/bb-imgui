// Blockbench .bbtheme (JSON) -> ImGui style + a resolved colour palette.
#pragma once

#include "imgui.h"

#include <string>

namespace theme {

// A fully-resolved colour palette (mirrors Blockbench's theme colour keys).
struct Palette {
    ImVec4 ui;            // panel background
    ImVec4 back;          // deeper background (toolbars, activity bar)
    ImVec4 deep;          // deepest (inputs, viewport) — Blockbench `dark`
    ImVec4 border;        // borders / separators
    ImVec4 selected;      // selection / active background
    ImVec4 button;        // button background
    ImVec4 bright_ui;     // dropdown-menu background (bright even in dark mode)
    ImVec4 bright_ui_text;
    ImVec4 accent;        // accent / focus colour
    ImVec4 frame;         // window frame
    ImVec4 text;          // primary text
    ImVec4 light;         // emphasised text
    ImVec4 accent_text;   // text on top of `accent`
    ImVec4 subtle_text;   // secondary / muted text
    ImVec4 grid;
    ImVec4 wireframe;
    ImVec4 checkerboard;
    bool   is_dark = true;
};

// Font sizes, in points. Blockbench uses 16px Assistant (a compact face);
// these are tuned to match that visual weight.
namespace size {
inline constexpr float SMALL = 13.0f;
inline constexpr float BODY = 15.0f;
inline constexpr float HEADING = 17.0f;
inline constexpr float MONO = 13.0f;
inline constexpr float MENU_POINT = 17.0f; // Blockbench li.menu_bar_point font-size
inline constexpr float MENU_ITEM = 14.0f;  // rows inside a dropdown
inline constexpr float WORDMARK = 19.0f;   // Blockbench #corner_logo font-size (1.2em)
} // namespace size

inline constexpr float RADIUS = 4.0f;

// Load a palette. Resolution order:
//   1. $APP_THEME (path to a .bbtheme)
//   2. <exe dir>/assets/blockbench-dark.bbtheme
//   3. the built-in Blockbench dark default
Palette load();

// Push `p` into ImGui::GetStyle() and remember it for palette().
void apply(const Palette& p);

// The palette last passed to apply() (built-in default before that).
const Palette& palette();

// Parse "#rgb" / "#rgba" / "#rrggbb" / "#rrggbbaa". Returns false on bad input.
bool parse_hex(const std::string& s, ImVec4& out);

} // namespace theme
