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

// Font sizes.
//
// CSS `font-size` maps the font's em square (1000 units for Assistant) to N px.
// ImGui's `size_pixels` maps the ascent-to-descent height (1308 units for
// Assistant) to N px. So a CSS `17px` equals an ImGui size of 17 * 1308/1000.
// These constants are Blockbench's CSS px, pre-multiplied by that factor.
namespace size {
inline constexpr float CSS = 1.308f; // Assistant: unitsPerEm 1000, asc-desc 1308

inline constexpr float SMALL = 13.0f * CSS;      // Blockbench .small_text ~0.84em
inline constexpr float BODY = 16.0f * CSS;       // Blockbench body 16px
inline constexpr float HEADING = 18.0f * CSS;    // section headings
inline constexpr float MONO = 14.0f * CSS;       // code
inline constexpr float MENU_POINT = 17.0f * CSS; // Blockbench li.menu_bar_point
inline constexpr float MENU_ITEM = 16.0f * CSS;  // Blockbench .contextMenu (inherits 16px)
inline constexpr float WORDMARK = 19.0f * CSS;   // Blockbench #corner_logo (1.2em)
} // namespace size

inline constexpr float RADIUS = 4.0f;

// Fixed per-axis colours (css/setup.css --color-axis-{x,y,z}). Not
// themeable in real Blockbench — used to tint the corner of each axis's
// number field in the Transform panel, matching the viewport gizmo colours.
namespace axis {
inline const ImVec4 X = ImVec4(0xff / 255.0f, 0x12 / 255.0f, 0x42 / 255.0f, 1.0f);
inline const ImVec4 Y = ImVec4(0x23 / 255.0f, 0xd4 / 255.0f, 0x00 / 255.0f, 1.0f);
inline const ImVec4 Z = ImVec4(0x08 / 255.0f, 0x94 / 255.0f, 0xed / 255.0f, 1.0f);
} // namespace axis

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
