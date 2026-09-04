// Blockbench-styled widgets built on top of Dear ImGui.
#pragma once

#include "imgui.h"

#include <string>

namespace bb {

inline constexpr float PANEL_HEADER_H = 30.0f;
inline constexpr float ROW_H = 30.0f;

// --- panel chrome --------------------------------------------------------
void panel_header(const char* title);
bool begin_panel(const char* name);
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

} // namespace bb
