#include "gallery.h"

#include "bb.h"
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

// Live-example state.
struct State {
    bool   toggle_a = true;
    bool   check_a = true, check_b = false;
    int    radio = 1;
    int    segment = 0;
    int    combo = 2;
    float  slider = 0.4f;
    double num = 12.5;
    float  vec[3] = {1.0f, 0.0f, -2.5f};
    float  color[4] = {0.24f, 0.56f, 1.0f, 1.0f};
    std::string text = "cube";
    std::string query;
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
    {"Buttons", "Button", "bb::button(\"Add Cube\");",
     "A standard push button.",
     []{ bb::button("Add Cube"); ImGui::SameLine(); bb::button("Add Group"); }},

    {"Buttons", "Primary button", "bb::primary_button(\"Confirm\");",
     "Accent-filled button for the main action in a dialog.",
     []{ bb::primary_button("Confirm"); ImGui::SameLine(); bb::button("Cancel"); }},

    {"Buttons", "Icon button", "bb::icon_button(ICON_BRUSH, /*active=*/true);",
     "Compact 30x28 icon button. `active` draws a 2px accent underline "
     "(used for the selected tool).",
     []{ static int tool = 0;
         const char* ic[] = {ICON_BRUSH, ICON_MOVE, ICON_RESIZE, ICON_PIVOT};
         for (int i = 0; i < 4; i++) { if (i) ImGui::SameLine(0, 2);
             if (bb::icon_button(ic[i], tool == i)) tool = i; } }},

    {"Buttons", "Toggle", "bb::toggle(\"Snap to grid\", &on);",
     "A pill on/off switch. Returns true on the frame it changed.",
     []{ bb::toggle("Snap to grid", &g.toggle_a);
         bb::toggle("Local space", &g.check_b); }},

    {"Numeric", "NumSlider", "bb::NumOpts o;\no.step = 0.25; o.decimals = 2;\n"
     "bb::num_slider(\"pos.x\", &value, o);",
     "Blockbench's signature number field: drag left/right to scrub, scroll to "
     "nudge, double-click to type.",
     []{ bb::NumOpts o; o.step = 0.25; o.decimals = 2;
         bb::num_slider("posx", &g.num, o); }},

    {"Numeric", "Vec3", "float xyz[3];\nbb::vec3(\"position\", xyz);",
     "Three labelled NumSliders in a row (X / Y / Z).",
     []{ bb::vec3("position", g.vec); }},

    {"Numeric", "Slider", "bb::slider_float(\"opacity\", &v, 0.0f, 1.0f);",
     "A bounded value slider.",
     []{ bb::slider_float("opacity", &g.slider, 0.0f, 1.0f); }},

    {"Inputs", "Text input", "bb::input_text(\"name\", &str);",
     "Single-line text field bound to a std::string.",
     []{ bb::input_text("name", &g.text); }},

    {"Inputs", "Search", "bb::search(\"outliner\", &query);",
     "Text field with a leading search icon and placeholder.",
     []{ bb::search("outliner", &g.query); }},

    {"Inputs", "Combo", "const char* items[] = {\"Edit\",\"Paint\",\"Animate\"};\n"
     "bb::combo(\"mode\", &current, items, 3);",
     "Dropdown selection.",
     []{ static const char* items[] = {"Edit", "Paint", "Animate", "Display"};
         bb::combo("mode", &g.combo, items, 4); }},

    {"Inputs", "Segmented", "const char* seg[] = {\"Object\",\"Edge\",\"Face\"};\n"
     "bb::segmented(\"select\", &mode, seg, 3);",
     "Connected segmented control; the active segment is accent-filled.",
     []{ static const char* seg[] = {"Object", "Edge", "Face"};
         bb::segmented("select", &g.segment, seg, 3); }},

    {"Inputs", "Checkbox", "bb::checkbox(\"Visible\", &v);",
     "A boolean checkbox.",
     []{ bb::checkbox("Visible", &g.check_a); bb::checkbox("Locked", &g.check_b); }},

    {"Inputs", "Radio", "bb::radio(\"Local\", &space, 0);\n"
     "bb::radio(\"Global\", &space, 1);",
     "Mutually-exclusive options sharing one int.",
     []{ bb::radio("Local", &g.radio, 0); ImGui::SameLine();
         bb::radio("Global", &g.radio, 1); }},

    {"Inputs", "Color", "bb::color_edit(\"tint\", rgba);",
     "Colour picker with hex entry and an alpha bar.",
     []{ bb::color_edit("tint", g.color); }},

    {"Feedback", "Info", "bb::info(\"Model saved to project.\");",
     "An inline informational line with an accent icon.",
     []{ bb::info("Model saved to project."); }},

    {"Feedback", "Warning", "bb::warning(\"3 faces have no UV mapping.\");",
     "An inline warning line with an amber icon.",
     []{ bb::warning("3 faces have no UV mapping."); }},

    {"Feedback", "Progress", "bb::progress(0.62f, \"62%\");",
     "A determinate progress bar with an optional overlay label.",
     []{ static float t = 0.0f; t += ImGui::GetIO().DeltaTime * 0.15f;
         if (t > 1.0f) t = 0.0f;
         char b[8]; std::snprintf(b, sizeof(b), "%.0f%%", t * 100);
         bb::progress(t, b); }},

    {"Feedback", "Spinner", "bb::spinner();",
     "An indeterminate activity spinner.",
     []{ bb::spinner(); ImGui::SameLine(); ImGui::AlignTextToFramePadding();
         ImGui::TextDisabled("Loading\xe2\x80\xa6"); }},

    {"Feedback", "Kbd", "bb::kbd(\"Ctrl\"); bb::kbd(\"Z\");",
     "Keycap badges, rendered inline.",
     []{ bb::kbd("Ctrl"); ImGui::SameLine(0, 4); bb::kbd("Shift");
         ImGui::SameLine(0, 4); bb::kbd("Z"); }},

    {"Structure", "Collapsing", "if (bb::collapsing(\"Transform\")) { ... }",
     "A collapsible section header for grouping panel content.",
     []{ if (bb::collapsing("Transform")) {
             ImGui::Indent(8);
             bb::field_label("Pivot point");
             bb::vec3("pivot", g.vec);
             ImGui::Unindent(8);
         } }},
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
