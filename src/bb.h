// Blockbench-styled widgets built on top of Dear ImGui.
#pragma once

#include "imgui.h"

#include <string>

namespace bb {

inline constexpr float PANEL_HEADER_H = 32.0f; // Blockbench #center h3.panel_handle
inline constexpr float ROW_H = 30.0f;          // Blockbench .bar

// --- panel chrome --------------------------------------------------------
void panel_header(const char* title);
// `show_header=false` skips the grey title banner — real Blockbench's main
// Left/Workspace/Right columns don't have one; their own sub-panels
// (TEXTURES, TRANSFORM, OUTLINER, ...) already act as headers.
bool begin_panel(const char* name, bool show_header = true);
void end_panel();

// A left-aligned dim caption above a control (Blockbench field labels).
void field_label(const char* text);

// --- buttons ------------------------------------------------------------
bool button(const char* label, ImVec2 size = ImVec2(0, 0));
bool primary_button(const char* label, ImVec2 size = ImVec2(0, 0)); // accent fill
// 30x28 icon button; `active` draws a 2px accent underline.
bool icon_button(const char* icon_glyph, bool active = false);
// A pill on/off switch. Returns true when toggled.
bool toggle(const char* label, bool* v);

// --- numeric ----------------------------------------------------------
struct NumOpts {
    double step = 0.1;
    double min = 0.0, max = 0.0; // min==max => unbounded
    int    decimals = 2;
    float  width = 0.0f;         // 0 => fill
    const char* suffix = nullptr;
};
// Blockbench's signature draggable number field. Drag to scrub, double-click to
// type. Returns true if the value changed this frame.
bool num_slider(const char* id, double* v, const NumOpts& o = {});
// X/Y/Z row of num_sliders. Returns true if any changed.
bool vec3(const char* id, float v[3]);

// One field of a Transform-panel row: a num_slider with a small coloured
// triangle in its top-left corner (theme::axis::X/Y/Z), matching
// Blockbench's Position/Size/Pivot/Rotation fields — no letter label, the
// colour alone says which axis. `axis` is 0=X, 1=Y, 2=Z.
bool axis_field(const char* id, int axis, double* v, float width = 0.0f);
// A full Transform-panel row: a field_label followed by 3 axis_fields.
// Returns true if any of the three changed.
bool transform_row(const char* label, double v[3]);

// --- inputs -----------------------------------------------------------
bool input_text(const char* id, std::string* s, const char* hint = nullptr);
bool search(const char* id, std::string* s);
bool combo(const char* id, int* current, const char* const items[], int count);
bool checkbox(const char* label, bool* v);
bool radio(const char* label, int* current, int value);
// Connected segmented control; returns true if the selection changed.
bool segmented(const char* id, int* current, const char* const labels[], int count);
bool slider_float(const char* id, float* v, float mn, float mx, const char* fmt = "%.2f");
bool color_edit(const char* id, float col[4], bool alpha = true);

// --- feedback -------------------------------------------------------
void info(const char* text);
void warning(const char* text);
void progress(float frac, const char* overlay = nullptr);
void spinner(float radius = 9.0f);
void kbd(const char* text); // keycap badge (inline)

// A collapsible section (Blockbench sidebar group). Returns true if open.
bool collapsing(const char* label, bool default_open = true);

// --- textures / UV --------------------------------------------------------
// A 48px Blockbench texture-list row (css/panels.css .texture): a coloured
// 48x48 thumbnail placeholder (no real image decoding here), the filename,
// and a subtle resolution caption. Returns true when clicked.
bool texture_row(const char* name, const char* dims, ImVec4 thumb_color, bool selected);

// A checkerboard fill for the given size at the current cursor position —
// Blockbench's transparency/UV-canvas background (--color-checkerboard
// alternating with the panel's own background).
void checkerboard(ImVec2 size, float cell = 8.0f);

// The 3D viewport's static backdrop: a deep background, a simple grid, and
// the bottom-right XYZ axis indicator cluster — standing in for the real
// (unimplemented) 3D scene render.
void viewport_placeholder(ImVec2 size);

// --- outliner -----------------------------------------------------------
// One row of the Outliner tree (Blockbench #cubes_list .outliner_object).
// `leaf=false` draws a folder icon and a disclosure arrow (call for each
// child then outliner_pop() if this returns true); `leaf=true` draws a cube
// icon and never expands. The eye icon toggles `*visible` when non-null.
bool outliner_node(const char* label, bool leaf, bool selected, bool* visible,
                   const char* id = nullptr);
void outliner_pop();

} // namespace bb
