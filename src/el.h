// Element UI-styled widgets. Independent of bb:: (Blockbench) — this is the
// "elementui-components" branch's own visual language: Element's semantic
// button colours, pill/circle shapes, InputNumber steppers, Rate stars, etc.
//
// Element's own Form controls that don't need a different look from what
// bb:: already draws (Input, Select, Radio, Checkbox, Switch, Slider) are not
// duplicated here — the gallery calls straight into bb:: for those, just
// under an Element-style label. Everything Element-specific that bb:: has no
// equivalent for lives here.
#pragma once

#include "imgui.h"

namespace el {

enum class ButtonType { Default, Primary, Success, Warning, Danger, Info, Text };

struct ButtonOpts {
    bool plain = false;
    bool round = false;
    bool circle = false;
    bool disabled = false;
    bool loading = false;
    const char* icon = nullptr; // optional leading icon glyph
    float height = 32.0f;
};

// Returns true the frame it's clicked (never true if disabled/loading).
bool button(const char* label, ButtonType type = ButtonType::Default,
           const ButtonOpts& o = {});

// A bordered number field with - / + steppers (Element's el-input-number).
bool input_number(const char* id, double* v, double step = 1.0, double min = 0.0,
                  double max = 0.0, int decimals = 0);

// A row of stars; `value` is 0..max_stars (fractional allowed but not drawn
// half-filled unless `allow_half`). Returns true when changed.
bool rate(const char* id, float* value, int max_stars = 5, bool allow_half = false);

enum class TagType { Default, Success, Warning, Danger, Info };
// A small pill label. `closable` draws an "x"; returns true the frame it's
// clicked (the caller removes the tag).
bool tag(const char* text, TagType type = TagType::Default, bool plain = true,
        bool closable = false);

// A numeric/dot badge, drawn at the current cursor position overlapping
// whatever was drawn immediately before it (call right after the anchor
// widget, before advancing the cursor). `count <= 0` draws a dot.
void badge(int count, bool is_dot = false, int max = 99);

// A circular avatar with initials (no image loader here).
void avatar(const char* initials, float size = 32.0f);

enum class AlertType { Success, Warning, Danger, Info };
void alert(const char* title, AlertType type, const char* description = nullptr,
          bool* open = nullptr);

// A horizontal rule, optionally with centred text.
void divider(const char* text = nullptr);

// A white bordered card; `header` may be null. Content is drawn by `body`.
void card_begin(const char* header);
void card_end();
template <typename Fn>
void card(const char* header, Fn&& body) {
    card_begin(header);
    body();
    card_end();
}

// ---------------------------------------------------------------------------
// Data (Batch 2)
// ---------------------------------------------------------------------------

// A bordered, striped data table (el-table). `rows`/`row_count` are
// row_count*col_count cell strings laid out row-major. No sorting/selection
// in this pass.
void table(const char* id, const char* const* headers, int col_count,
          const char* const* rows, int row_count);

// A ring progress indicator with a centred percentage label (el-progress
// type="circle"). `frac` is 0..1.
void progress_circle(float frac, float radius = 40.0f, const char* text = nullptr);

// One expandable row of an el-tree. Call for each node; if it returns true,
// draw the children (indent yourself) then call tree_pop(). `leaf` nodes
// have no disclosure arrow and can't be expanded.
bool tree_node(const char* label, bool leaf = false, bool selected = false);
void tree_pop();

// A row of page buttons with prev/next arrows. `current` is 1-based.
// Returns true the frame it changes.
bool pagination(const char* id, int* current, int total_pages);

} // namespace el
