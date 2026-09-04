// Blockbench-style menu-bar dropdowns (File / Edit / …).
//
// Flat menus for now: icon column, right-aligned shortcut, separators,
// checkmarks. Submenu rows draw a chevron (wiring is a follow-up).
#pragma once

#include "imgui.h"

namespace menu {

struct Item {
    const char* icon = nullptr;     // UTF-8 glyph, or nullptr
    const char* label = nullptr;    // nullptr => separator
    const char* shortcut = nullptr; // right-aligned, or nullptr
    bool        checked = false;
    bool        has_submenu = false;
    bool        enabled = true;
};

inline Item separator() { return Item{}; }

struct Menu {
    const char* name;
    const Item* items;
    int         count;
};

// Draw one menu-bar point (`m`, at `index`) as a `size`-big hit box at screen
// `pos`. Manages the shared open/hover-switch state and the dropdown popup.
// Returns the label of a leaf activated this frame, or nullptr.
const char* point(const Menu& m, int index, ImVec2 pos, ImVec2 size);

// True while any dropdown is open (title bar uses this to keep its look).
bool any_open();

} // namespace menu
