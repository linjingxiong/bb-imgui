#include "gallery.h"

#include "bb.h"
#include "el.h"
#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include "imgui.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

namespace gallery {
namespace {

// Live-example state. Entries below are grouped to match Element UI's own
// component taxonomy ("按 Element UI 分类分批次") — Batch 1 is Form, Batch 2 is
// Data, Batch 3 is Notice.
struct State {
    bool   toggle_a = true;
    bool   check_a = true, check_b = false;
    int    radio = 1;
    int    combo = 2;
    float  slider = 0.4f;
    double input_num = 3.0;
    float  rate_val = 3.0f;
    bool   btn_loading = false;
    std::string text = "cube";

    bool   tag_closable[3] = {true, true, true};
    int    pagination_page = 3;

    bool   alert_open = true;
    bool   loading_active = false;
    bool   msgbox_open = false;
    bool   msgbox_alert_open = false;

    int    tabs_current = 0;
    int    steps_current = 1;
    std::string dropdown_pick = "(none)";

    bool   dialog_open = false;
    bool   collapse_open[2] = {true, false};
};
State g;

struct Entry {
    const char* group;
    const char* name;
    const char* code;
    const char* note;
    std::function<void()> demo;
};

// clang-format off
const Entry ENTRIES[] = {
    // --- Button --------------------------------------------------------
    // Reproduces element.eleme.cn's Button "基础用法" example exactly: four
    // rows (solid / plain / round / circle-icon), each in the canonical
    // Default-Primary-Success-Info-Warning-Danger order.
    {"Button", "Basic usage",
     "el::button(\"Default\");\nel::button(\"Primary\", el::ButtonType::Primary);\n"
     "el::button(\"Success\", el::ButtonType::Success);\nel::button(\"Info\", el::ButtonType::Info);\n"
     "el::button(\"Warning\", el::ButtonType::Warning);\nel::button(\"Danger\", el::ButtonType::Danger);\n\n"
     "// o.plain / o.round / o.circle style a Button (ButtonOpts)",
     "Use `type`, `plain`, `round`, and `circle` to style a Button.",
     []{
         static const el::ButtonType TYPES[] = {
             el::ButtonType::Default, el::ButtonType::Primary, el::ButtonType::Success,
             el::ButtonType::Info,    el::ButtonType::Warning, el::ButtonType::Danger,
         };
         static const char* LABELS[] = {"Default", "Primary", "Success",
                                        "Info",    "Warning", "Danger"};
         // Row 1: solid.
         for (int i = 0; i < 6; i++) {
             if (i) ImGui::SameLine();
             el::button(LABELS[i], TYPES[i]);
         }
         ImGui::Dummy(ImVec2(0, 6));
         // Row 2: plain.
         el::ButtonOpts plain; plain.plain = true;
         for (int i = 0; i < 6; i++) {
             if (i) ImGui::SameLine();
             el::button(LABELS[i], TYPES[i], plain);
         }
         ImGui::Dummy(ImVec2(0, 6));
         // Row 3: round.
         el::ButtonOpts round; round.round = true;
         for (int i = 0; i < 6; i++) {
             if (i) ImGui::SameLine();
             el::button(LABELS[i], TYPES[i], round);
         }
         ImGui::Dummy(ImVec2(0, 6));
         // Row 4: circle + icon, no label (search/edit/check/info/star/delete —
         // no envelope glyph available in the merged icon set, ICON_INFO
         // substitutes for the "info"-type slot).
         static const char* ICONS[] = {ICON_SEARCH, ICON_EDIT, ICON_CHECK,
                                       ICON_INFO,   ICON_STAR, ICON_DELETE};
         el::ButtonOpts circle; circle.circle = true;
         for (int i = 0; i < 6; i++) {
             if (i) ImGui::SameLine();
             circle.icon = ICONS[i];
             el::button("", TYPES[i], circle);
         }
     }},

    {"Button", "Button (disabled / loading)",
     "el::ButtonOpts o; o.disabled = true;\nel::button(\"Confirm\", el::ButtonType::Primary, o);",
     "Disabled buttons ignore clicks and fade to 50% alpha; loading buttons show "
     "a spinner in place of the label.",
     []{ el::ButtonOpts d; d.disabled = true;
         el::button("Confirm", el::ButtonType::Primary, d); ImGui::SameLine();
         if (el::button("Toggle loading")) g.btn_loading = !g.btn_loading;
         ImGui::SameLine();
         el::ButtonOpts l; l.loading = g.btn_loading;
         el::button("Loading", el::ButtonType::Primary, l); }},

    {"Button", "Button (size)",
     "el::ButtonOpts o; o.size = el::ButtonSize::Small;\nel::button(\"Small\", el::ButtonType::Primary, o);",
     "Four sizes — Default/Medium/Small/Mini — each with its own padding, "
     "font-size, and (for Small/Mini) a slightly tighter corner radius, "
     "matching Element's var.scss exactly.",
     []{ el::ButtonOpts o;
         el::button("Default", el::ButtonType::Primary, o); ImGui::SameLine();
         o.size = el::ButtonSize::Medium;
         el::button("Medium", el::ButtonType::Primary, o); ImGui::SameLine();
         o.size = el::ButtonSize::Small;
         el::button("Small", el::ButtonType::Primary, o); ImGui::SameLine();
         o.size = el::ButtonSize::Mini;
         el::button("Mini", el::ButtonType::Primary, o); }},

    {"Button", "Button Group", "const char* labels[] = {\"Edit\",\"Copy\",\"Delete\"};\n"
     "el::button_group(labels, 3);",
     "Several buttons drawn as one connected pill — flush shared borders, "
     "square inner corners. Returns the index of whichever one was clicked.",
     []{ static const char* labels[] = {"Edit", "Copy", "Delete"};
         static const el::ButtonType types[] = {el::ButtonType::Default,
             el::ButtonType::Default, el::ButtonType::Danger};
         int i = el::button_group(labels, 3, types);
         if (i >= 0) el::message(labels[i], el::NoticeType::Info); }},

    // --- Form ------------------------------------------------------------
    {"Form", "Input", "bb::input_text(\"name\", &str);",
     "Single-line text field bound to a std::string.",
     []{ bb::input_text("name", &g.text); }},

    {"Form", "InputNumber", "double v = 3;\nel::input_number(\"count\", &v, 1.0, 0.0, 10.0);",
     "A bordered field with -/+ steppers (Element's el-input-number).",
     []{ el::input_number("count", &g.input_num, 1.0, 0.0, 10.0); }},

    {"Form", "Select", "const char* items[] = {\"Edit\",\"Paint\",\"Animate\"};\n"
     "bb::combo(\"mode\", &current, items, 3);",
     "Dropdown selection.",
     []{ static const char* items[] = {"Edit", "Paint", "Animate", "Display"};
         bb::combo("mode", &g.combo, items, 4); }},

    {"Form", "Radio / RadioGroup", "bb::radio(\"Local\", &space, 0);\n"
     "bb::radio(\"Global\", &space, 1);",
     "Mutually-exclusive options sharing one int.",
     []{ bb::radio("Local", &g.radio, 0); ImGui::SameLine();
         bb::radio("Global", &g.radio, 1); }},

    {"Form", "Checkbox / CheckboxGroup", "bb::checkbox(\"Visible\", &v);",
     "Independent booleans.",
     []{ bb::checkbox("Visible", &g.check_a); bb::checkbox("Locked", &g.check_b); }},

    {"Form", "Switch", "bb::toggle(\"Snap to grid\", &on);",
     "A pill on/off switch. Returns true on the frame it changed.",
     []{ bb::toggle("Snap to grid", &g.toggle_a); }},

    {"Form", "Slider", "bb::slider_float(\"opacity\", &v, 0.0f, 1.0f);",
     "A bounded value slider.",
     []{ bb::slider_float("opacity", &g.slider, 0.0f, 1.0f); }},

    {"Form", "Rate", "float v = 3;\nel::rate(\"quality\", &v);",
     "A row of stars; click to set the value.",
     []{ el::rate("quality", &g.rate_val); }},

    // --- Data --------------------------------------------------------
    {"Data", "Table", "const char* head[] = {\"Name\",\"Type\"};\n"
     "const char* rows[] = {\"Cube\",\"Mesh\", \"Group\",\"Bone\"};\n"
     "el::table(\"t\", head, 2, rows, 2);",
     "A bordered, striped data table with a light grey header.",
     []{ static const char* head[] = {"Name", "Type", "Visible"};
         static const char* rows[] = {
             "Cube",  "Mesh", "Yes",
             "Group", "Bone", "Yes",
             "Torso", "Bone", "No",
         };
         el::table("demo", head, 3, rows, 3); }},

    {"Data", "Tag", "el::tag(\"Tag One\", el::TagType::Success);",
     "A small pill label in one of five semantic colours; `closable` adds an "
     "\"x\" the caller can react to.",
     []{ static const el::TagType types[] = {el::TagType::Default, el::TagType::Success,
             el::TagType::Warning};
         static const char* names[] = {"Default", "Success", "Warning"};
         for (int i = 0; i < 3; i++) {
             if (i) ImGui::SameLine(0, 6);
             if (!g.tag_closable[i]) continue;
             if (el::tag(names[i], types[i], true, true)) g.tag_closable[i] = false;
         }
         if (!g.tag_closable[0] && !g.tag_closable[1] && !g.tag_closable[2]) {
             ImGui::SameLine();
             if (bb::button("Reset")) for (bool& b : g.tag_closable) b = true;
         } }},

    {"Data", "Progress", "bb::progress(0.72f, \"72%\");\nel::progress_circle(0.72f);",
     "Line and circle variants, both driven by a 0..1 fraction.",
     []{ static float t = 0.0f; t += ImGui::GetIO().DeltaTime * 0.1f; if (t > 1.0f) t = 0.0f;
         char b[8]; std::snprintf(b, sizeof(b), "%.0f%%", t * 100);
         bb::progress(t, b);
         ImGui::Dummy(ImVec2(0, 8));
         el::progress_circle(t); }},

    {"Data", "Tree", "if (el::tree_node(\"Group\")) {\n    el::tree_node(\"Cube\", /*leaf=*/true);\n    el::tree_pop();\n}",
     "An indented, expandable outliner-style tree. Click a row with children "
     "to toggle it.",
     []{ if (el::tree_node("Model")) {
             ImGui::Indent(14);
             if (el::tree_node("Group", false, true)) {
                 ImGui::Indent(14);
                 el::tree_node("Cube", true);
                 el::tree_node("Cube.001", true);
                 ImGui::Unindent(14);
                 el::tree_pop();
             }
             el::tree_node("Torso", true);
             ImGui::Unindent(14);
             el::tree_pop();
         } }},

    {"Data", "Pagination", "int page = 3;\nel::pagination(\"p\", &page, 10);",
     "Prev/next arrows plus a window of nearby page numbers; the active page "
     "is accent-filled.",
     []{ el::pagination("demo", &g.pagination_page, 10); }},

    {"Data", "Badge", "el::button(\"Messages\");\nel::badge(5);",
     "A numeric/dot badge anchored to the top-right corner of whatever was "
     "drawn immediately before it.",
     []{ el::button("Messages"); el::badge(5); ImGui::SameLine(0, 24);
         el::button("New"); el::badge(0, true); }},

    {"Data", "Avatar", "el::avatar(\"JD\");",
     "A circular initials avatar; colour is derived from the initials.",
     []{ el::avatar("JD"); ImGui::SameLine(0, 8); el::avatar("AB"); ImGui::SameLine(0, 8);
         el::avatar("XY"); }},

    {"Data", "Card", "el::card(\"Title\", [] {\n    ImGui::TextUnformatted(\"Body content\");\n});",
     "A white bordered container with an optional header and rule.",
     []{ el::card("Cube", [] {
             ImGui::TextUnformatted("32 x 32 x 32");
             ImGui::TextDisabled("Last edited 2m ago");
         }); }},

    // --- Notice --------------------------------------------------------
    {"Notice", "Alert", "el::alert(\"Success\", el::AlertType::Success,\n"
     "          \"Model saved.\", &open);",
     "An inline banner with an icon, optional description, and an optional "
     "close button.",
     []{ if (g.alert_open)
             el::alert("Model saved successfully.", el::AlertType::Success,
                       "The .bbmodel file was written to disk.", &g.alert_open);
         else if (bb::button("Reset")) g.alert_open = true; }},

    {"Notice", "Loading", "el::loading_overlay(rmin, rmax, active);",
     "A semi-transparent overlay + spinner drawn over a region while "
     "`active`. Draw it right after the content it should mask.",
     []{ if (bb::button(g.loading_active ? "Stop" : "Start"))
             g.loading_active = !g.loading_active;
         ImGui::Dummy(ImVec2(0, 8));
         ImVec2 p0 = ImGui::GetCursorScreenPos();
         ImVec2 p1(p0.x + 220, p0.y + 80);
         ImGui::GetWindowDrawList()->AddRectFilled(p0, p1,
             ImGui::ColorConvertFloat4ToU32(theme::palette().deep), theme::RADIUS);
         ImGui::Dummy(ImVec2(220, 80));
         el::loading_overlay(p0, p1, g.loading_active); }},

    {"Notice", "Message", "el::message(\"Saved!\", el::NoticeType::Success);",
     "A transient toast, centred at the top of the screen, that fades out "
     "on its own.",
     []{ if (bb::button("Success")) el::message("This is a success message.", el::NoticeType::Success);
         ImGui::SameLine();
         if (bb::button("Warning")) el::message("This is a warning message.", el::NoticeType::Warning);
         ImGui::SameLine();
         if (bb::button("Error")) el::message("This is an error message.", el::NoticeType::Danger); }},

    {"Notice", "MessageBox", "el::MessageBoxResult r = el::message_box(\n"
     "    \"confirm\", \"Confirm\", \"Delete this?\", &open);",
     "A modal dialog for confirm (OK/Cancel) or alert (OK only) flows.",
     []{ if (bb::button("Confirm box")) g.msgbox_open = true;
         auto r = el::message_box("confirm_demo", "Confirm", "This will permanently "
             "delete the selected item. Continue?", &g.msgbox_open);
         if (r == el::MessageBoxResult::Confirm) el::message("Deleted.", el::NoticeType::Success);
         else if (r == el::MessageBoxResult::Cancel) el::message("Cancelled.", el::NoticeType::Info);

         ImGui::SameLine();
         if (bb::button("Alert box")) g.msgbox_alert_open = true;
         el::message_box("alert_demo", "Notice", "This action can't be undone.",
                         &g.msgbox_alert_open, /*show_cancel=*/false); }},

    {"Notice", "Notification", "el::notify(\"Title\", \"Description text.\",\n"
     "          el::NoticeType::Info);",
     "A transient card in the top-right corner with a title and description.",
     []{ if (bb::button("Notify"))
             el::notify("New version available", "bb-imgui 0.2.0 is ready to install.",
                       el::NoticeType::Info); }},

    // --- Navigation ------------------------------------------------------
    {"Navigation", "Tabs", "const char* labels[] = {\"Detail\",\"Rules\",\"Reviews\"};\n"
     "el::tabs(\"t\", &current, labels, 3);",
     "A row of text tabs on a baseline rule; the active tab gets an accent "
     "underline.",
     []{ static const char* labels[] = {"Detail", "Rules", "Reviews"};
         el::tabs("demo", &g.tabs_current, labels, 3);
         ImGui::Dummy(ImVec2(0, 8));
         ImGui::TextDisabled("content for \"%s\"", labels[g.tabs_current]); }},

    {"Navigation", "Breadcrumb", "const char* crumbs[] = {\"Model\",\"Group\",\"Cube\"};\n"
     "el::breadcrumb(crumbs, 3);",
     "Clickable path segments with a chevron separator; the last segment is "
     "plain text.",
     []{ static const char* crumbs[] = {"Model", "Group", "Cube"};
         el::breadcrumb(crumbs, 3); }},

    {"Navigation", "Steps", "const char* labels[] = {\"Upload\",\"Configure\",\"Done\"};\n"
     "el::steps(labels, 3, current);",
     "A horizontal progress indicator; steps before `current` are marked "
     "done with a checkmark.",
     []{ static const char* labels[] = {"Upload", "Configure", "Done"};
         el::steps(labels, 3, g.steps_current);
         if (bb::button("Back") && g.steps_current > 0) g.steps_current--;
         ImGui::SameLine();
         if (bb::button("Next") && g.steps_current < 2) g.steps_current++; }},

    {"Navigation", "Dropdown", "const char* items[] = {\"Edit\",\"Duplicate\",\"Delete\"};\n"
     "int i = el::dropdown(\"d\", \"Actions\", items, 3);",
     "A button that opens a menu of items below it; returns the clicked "
     "index for one frame.",
     []{ static const char* items[] = {"Edit", "Duplicate", "Delete"};
         int i = el::dropdown("demo", "Actions", items, 3);
         if (i >= 0) g.dropdown_pick = items[i];
         ImGui::SameLine();
         ImGui::TextDisabled("picked: %s", g.dropdown_pick.c_str()); }},

    // --- Others ----------------------------------------------------------
    {"Others", "Dialog", "el::dialog(\"d\", \"Title\", &open, [] {\n"
     "    ImGui::TextUnformatted(\"Body content\");\n});",
     "A modal dialog with a title bar and close button around free-form "
     "content.",
     []{ if (bb::button("Open dialog")) g.dialog_open = true;
         el::dialog("demo", "Rename cube", &g.dialog_open, [] {
             static std::string name = "Cube";
             bb::field_label("Name");
             bb::input_text("name", &name);
             ImGui::Dummy(ImVec2(0, 12));
             ImGui::SetCursorPosX(ImGui::GetWindowWidth() - 160);
             if (el::button("Cancel")) g.dialog_open = false;
             ImGui::SameLine(0, 8);
             if (el::button("Confirm", el::ButtonType::Primary)) g.dialog_open = false;
         }); }},

    {"Others", "Tooltip", "bb::button(\"Hover me\");\nel::tooltip(\"Helpful text\");",
     "A small dark tooltip shown when the previous item is hovered.",
     []{ bb::button("Hover me"); el::tooltip("This is a tooltip."); }},

    {"Others", "Popover", "bb::button(\"Click me\");\n"
     "el::popover(\"p\", [] { ImGui::TextUnformatted(\"Rich content here\"); });",
     "A white bordered popover anchored under the trigger, opened by "
     "clicking it.",
     []{ bb::button("Click me");
         el::popover("demo", [] {
             ImGui::PushFont(nullptr, theme::size::SMALL);
             ImGui::TextUnformatted("Popover title");
             ImGui::PopFont();
             ImGui::TextDisabled("Any content can go here.");
         }); }},

    {"Others", "Collapse", "if (el::collapse_item(\"Section\", &open)) {\n"
     "    ImGui::TextUnformatted(\"...\");\n    el::collapse_pop();\n}",
     "An accordion-style panel; each panel tracks its own open state.",
     []{ if (el::collapse_item("Consistency", &g.collapse_open[0])) {
             ImGui::TextWrapped("Consistent with real life: in line with the "
                                "process and logic of real life.");
             el::collapse_pop();
         }
         if (el::collapse_item("Feedback", &g.collapse_open[1])) {
             ImGui::TextWrapped("Operation feedback: enable users to clearly "
                                "perceive their operations.");
             el::collapse_pop();
         } }},

    {"Others", "Timeline", "el::TimelineItem items[] = {\n"
     "    {\"2026-09-01\", \"Created\"},\n    {\"2026-09-03\", \"Reviewed\"},\n};\n"
     "el::timeline(items, 2);",
     "A vertical dot-and-line list of dated events.",
     []{ static const el::TimelineItem items[] = {
             {"2026-09-05", "Batch 5 shipped", "Others: Dialog, Tooltip, Popover, Collapse, Timeline."},
             {"2026-09-04", "Element theme", "Switched the palette to Element's blue/white light theme."},
             {"2026-09-01", "Branch created", nullptr},
         };
         el::timeline(items, 3); }},

    {"Others", "Divider", "el::divider();\nel::divider(\"Section\");",
     "A horizontal rule, optionally with centred text.",
     []{ ImGui::TextUnformatted("Above"); el::divider();
         ImGui::TextUnformatted("Between"); el::divider("More");
         ImGui::TextUnformatted("Below"); }},
};
constexpr int COUNT = (int)(sizeof(ENTRIES) / sizeof(ENTRIES[0]));

int g_selected = 0;

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

} // namespace

void list() {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float row_h = 28.0f;
    const char* cur_group = nullptr;

    for (int i = 0; i < COUNT; i++) {
        const Entry& e = ENTRIES[i];
        if (!cur_group || std::strcmp(cur_group, e.group) != 0) {
            cur_group = e.group;
            ImGui::Dummy(ImVec2(0, i == 0 ? 0.0f : 8.0f));
            ImGui::PushFont(fonts::medium(), theme::size::SMALL);
            std::string up(cur_group);
            for (char& c : up) c = (char)std::toupper((unsigned char)c);
            ImGui::TextColored(p.subtle_text, "%s", up.c_str());
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 2));
        }

        ImVec2 pos = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        ImGui::PushID(i);
        ImGui::InvisibleButton("row", ImVec2(w, row_h));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            g_selected = i;
        ImGui::PopID();

        bool sel = (g_selected == i);
        if (sel)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row_h),
                              u32(ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.16f)));
        else if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + row_h),
                              u32(ImVec4(p.light.x, p.light.y, p.light.z, 0.05f)));
        if (sel)
            dl->AddRectFilled(pos, ImVec2(pos.x + 2, pos.y + row_h), u32(p.accent));

        ImVec2 ts = ImGui::CalcTextSize(e.name);
        dl->AddText(ImVec2(pos.x + 12, pos.y + (row_h - ts.y) * 0.5f),
                    u32(sel ? p.light : (hovered ? p.light : p.text)), e.name);
    }
}

void detail() {
    const theme::Palette& p = theme::palette();
    if (g_selected < 0 || g_selected >= COUNT)
        return;
    const Entry& e = ENTRIES[g_selected];

    ImGui::PushFont(fonts::medium(), theme::size::HEADING);
    ImGui::TextUnformatted(e.name);
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::PushStyleColor(ImGuiCol_Text, p.subtle_text);
    ImGui::TextWrapped("%s", e.note);
    ImGui::PopStyleColor();
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(0, 10));

    // --- live example, in a framed area --------------------------------
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::TextColored(p.subtle_text, "EXAMPLE");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.deep);
    ImGui::PushStyleColor(ImGuiCol_Border, p.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, theme::RADIUS);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 16));
    ImGui::BeginChild("example", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushItemWidth(280.0f);
    e.demo();
    ImGui::PopItemWidth();
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    ImGui::Dummy(ImVec2(0, 12));

    // --- usage --------------------------------------------------------
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::TextColored(p.subtle_text, "USAGE");
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.back);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild("code", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    ImGui::PushFont(nullptr, theme::size::MONO);
    ImGui::PushStyleColor(ImGuiCol_Text, p.text);
    ImGui::TextUnformatted(e.code);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

} // namespace gallery
