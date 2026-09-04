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
// Element's four button sizes. Height isn't a fixed number in Element — it
// falls out of font-size + padding — so this drives padding/font-size/
// radius per theme-chalk's var.scss, and the actual pixel height follows
// from the measured text at that font size.
enum class ButtonSize { Default, Medium, Small, Mini };

struct ButtonOpts {
    ButtonSize size = ButtonSize::Default;
    bool plain = false;
    bool round = false;
    bool circle = false;
    bool disabled = false;
    bool loading = false;
    bool autofocus = false;      // focus this button when its window first appears
    const char* icon = nullptr;  // optional leading icon glyph
};

// Returns true the frame it's clicked (never true if disabled/loading).
bool button(const char* label, ButtonType type = ButtonType::Default,
           const ButtonOpts& o = {});

// `count` buttons drawn as one connected pill (el-button-group): flush
// borders between neighbours, square inner corners, rounded only on the two
// outer ends. `types`/`opts` are parallel arrays of length `count`, or null
// to use the default for every button. Returns the 0-based index of the
// button clicked this frame, or -1.
int button_group(const char* const* labels, int count, const ButtonType* types = nullptr,
                 const ButtonOpts* opts = nullptr);

// A 14x14px checkbox (Element's fixed size — deliberately independent of
// the global ImGuiStyle::FramePadding used to size Button/Input to 40px,
// which would otherwise blow this up to match). Returns true the frame it
// changes.
bool checkbox(const char* label, bool* v, bool disabled = false);

// A 14x14px radio button (same reasoning as checkbox() — Element's radio
// dot is a fixed size, independent of the global control-height padding).
// `*current` is shared by every radio() call in the group; this one is
// selected when `*current == value`. Returns true the frame it's picked.
bool radio(const char* label, int* current, int value, bool disabled = false);

// A bordered number field with - / + steppers (Element's el-input-number).
bool input_number(const char* id, double* v, double step = 1.0, double min = 0.0,
                  double max = 0.0, int decimals = 0);

// A row of stars; `value` is 0..max_stars (fractional allowed but not drawn
// half-filled unless `allow_half`). Returns true when changed.
bool rate(const char* id, float* value, int max_stars = 5, bool allow_half = false);

enum class TagType { Default, Success, Warning, Danger, Info };
// A small pill label. `plain` (default) is a light tinted background with
// coloured text/border; `plain = false` is a solid coloured fill with white
// text (Element's "dark" effect — more emphasis). `closable` draws an "x";
// returns true the frame it's clicked (the caller removes the tag).
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
// have no disclosure arrow and can't be expanded. Expand state is keyed by
// `id` if given, else by `label` — pass an explicit `id` when sibling
// nodes can share the same display text (e.g. two objects both named
// "Cube"), or their open/closed state will collide.
bool tree_node(const char* label, bool leaf = false, bool selected = false,
              const char* id = nullptr);
void tree_pop();

// A row of page buttons with prev/next arrows. `current` is 1-based.
// Returns true the frame it changes.
bool pagination(const char* id, int* current, int total_pages);

// ---------------------------------------------------------------------------
// Notice (Batch 3)
// ---------------------------------------------------------------------------

enum class NoticeType { Success, Warning, Danger, Info };

// Queues a transient, auto-dismissing toast centred at the top of the
// screen (el-message). Call once per user action (e.g. inside an `if
// (button(...))`), not every frame — each call enqueues one toast.
void message(const char* text, NoticeType type = NoticeType::Info, float duration = 3.0f);

// Queues a transient, auto-dismissing card in the top-right corner with a
// title and optional body (el-notification). Same call convention as
// message().
void notify(const char* title, const char* description = nullptr,
           NoticeType type = NoticeType::Info, float duration = 4.5f);

// Draws and ages every pending message()/notify() toast. Call exactly once
// per frame, after all other UI, so toasts draw on top of everything.
void render_notices();

// A semi-transparent overlay + spinner drawn over a screen-space rect when
// `active` (el-loading directive). Call right after drawing the content you
// want to mask, using its ImGui::GetItemRect{Min,Max}().
void loading_overlay(ImVec2 region_min, ImVec2 region_max, bool active);

enum class MessageBoxResult { None, Confirm, Cancel };
// A modal confirm/alert dialog (el-message-box). `*open` is both the
// trigger (set true to open it) and is cleared automatically once the user
// picks a button. `show_cancel = false` gives a single-button "alert" style
// box instead of a "confirm" style box.
MessageBoxResult message_box(const char* id, const char* title, const char* text,
                             bool* open, bool show_cancel = true);

// ---------------------------------------------------------------------------
// Navigation (Batch 4)
// ---------------------------------------------------------------------------

// A row of tab labels with an accent underline on the active tab, sitting
// on a full-width baseline rule (el-tabs). `current` is an in/out index.
// Returns true the frame it changes.
bool tabs(const char* id, int* current, const char* const* labels, int count);

// "Home > Products > Detail" — separators are drawn automatically, the
// last crumb is rendered as plain (non-clickable) text. Returns the
// 0-based index of a clicked crumb this frame, or -1.
int breadcrumb(const char* const* labels, int count);

// A horizontal step indicator (el-steps). `current` is the 0-based active
// step; steps before it are marked done (filled, checkmark). Purely
// presentational — no return value.
void steps(const char* const* labels, int count, int current);

// A button that opens a dropdown menu of `items` below it when clicked.
// Returns the 0-based index of the clicked item this frame, or -1.
int dropdown(const char* id, const char* label, const char* const* items, int count);

// ---------------------------------------------------------------------------
// Others (Batch 5)
// ---------------------------------------------------------------------------

// A modal dialog with a title bar (title text + close X) around free-form
// content (el-dialog). Only draws while `*open` is true; the close X (or
// dialog_end()'s caller) clears it. Mirrors card_begin/card_end/card.
bool dialog_begin(const char* id, const char* title, bool* open, float width = 420.0f);
void dialog_end();
template <typename Fn>
void dialog(const char* id, const char* title, bool* open, Fn&& body, float width = 420.0f) {
    if (dialog_begin(id, title, open, width)) {
        body();
        dialog_end();
    }
}

// A small dark tooltip shown when the previously-drawn item is hovered
// (el-tooltip). Call immediately after that item.
void tooltip(const char* text);

// A white bordered popover anchored under the previously-drawn item,
// opened by clicking it (el-popover). `body` draws the popover's content.
template <typename Fn>
void popover(const char* id, Fn&& body) {
    if (ImGui::IsItemClicked()) ImGui::OpenPopup(id);
    popover_style_push();
    if (ImGui::BeginPopup(id)) {
        body();
        ImGui::EndPopup();
    }
    popover_style_pop();
}
// (Style helpers behind popover<Fn>() above — not meant to be called
// directly, but declared here since the template needs them.)
void popover_style_push();
void popover_style_pop();

// One panel of an el-collapse accordion. Call for each panel; if it
// returns true, draw the panel body then call collapse_pop(). `*open`
// holds this panel's own expanded state (independent panels, not a
// single-open accordion). Pass an explicit `id` if two panels can share
// the same `title` text, same caveat as tree_node().
bool collapse_item(const char* title, bool* open, const char* id = nullptr);
void collapse_pop();

struct TimelineItem {
    const char* time;
    const char* title;
    const char* desc = nullptr;
};
// A vertical dot-and-line timeline (el-timeline).
void timeline(const TimelineItem* items, int count);

} // namespace el
