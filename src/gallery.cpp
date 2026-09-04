#include "gallery.h"

#include "bb.h"
#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include "imgui.h"

#include <cctype>
#include <functional>
#include <string>

namespace gallery {
namespace {

// State for the live examples.
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
    float  progress = 0.62f;
};
State g;

void section(const char* title) {
    const theme::Palette& p = theme::palette();
    ImGui::Dummy(ImVec2(0, 5));
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    std::string up(title);
    for (char& c : up) c = (char)toupper((unsigned char)c);
    ImGui::TextColored(p.subtle_text, "%s", up.c_str());
    ImGui::PopFont();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 mx = ImGui::GetItemRectMax();
    float cy = (ImGui::GetItemRectMin().y + mx.y) * 0.5f;
    float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    dl->AddLine(ImVec2(mx.x + 8, cy), ImVec2(right, cy),
                ImGui::ColorConvertFloat4ToU32(p.border), 1.0f);
    ImGui::Dummy(ImVec2(0, 1));
}

// One row: live widget (left) + usage snippet (right).
void row(const char* code, const std::function<void()>& widget) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(code);
    widget();
    ImGui::PopID();
    ImGui::TableNextColumn();
    if (code && code[0]) {
        ImGui::PushFont(nullptr, theme::size::MONO);
        ImGui::PushStyleColor(ImGuiCol_Text, theme::palette().subtle_text);
        ImGui::TextWrapped("%s", code);
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }
}

void sect(const char* title) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    section(title);
    ImGui::TableNextColumn();
}

} // namespace

void draw() {
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(10, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 5));
    if (!ImGui::BeginTable("gallery", 2,
                           ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
        ImGui::PopStyleVar(2);
        return;
    }
    ImGui::TableSetupColumn("widget", ImGuiTableColumnFlags_WidthStretch, 0.42f);
    ImGui::TableSetupColumn("usage", ImGuiTableColumnFlags_WidthStretch, 0.58f);

    // -- Buttons --------------------------------------------------------
    sect("Buttons");
    row("bb::button(\"Add Cube\");", [] { bb::button("Add Cube"); });
    row("bb::primary_button(\"Confirm\");", [] { bb::primary_button("Confirm"); });
    row("bb::icon_button(ICON_BRUSH, /*active*/ true);",
        [] { bb::icon_button(ICON_BRUSH, true); ImGui::SameLine();
             bb::icon_button(ICON_MOVE, false); ImGui::SameLine();
             bb::icon_button(ICON_RESIZE, false); });
    row("bb::toggle(\"Snap to grid\", &on);",
        [] { bb::toggle("Snap to grid", &g.toggle_a); });

    // -- Numeric ------------------------------------------------------
    sect("Numeric fields");
    row("bb::NumOpts o; o.step = 0.25; o.decimals = 2;\n"
        "bb::num_slider(\"pos.x\", &value, o);  // drag / double-click",
        [] { bb::NumOpts o; o.step = 0.25; o.decimals = 2;
             bb::num_slider("pos.x", &g.num, o); });
    row("float xyz[3];\nbb::vec3(\"position\", xyz);",
        [] { bb::vec3("position", g.vec); });
    row("bb::slider_float(\"opacity\", &v, 0, 1);",
        [] { bb::slider_float("##op", &g.slider, 0.0f, 1.0f); });

    // -- Inputs -----------------------------------------------------
    sect("Inputs & selection");
    row("bb::input_text(\"name\", &str);",
        [] { bb::input_text("##name", &g.text); });
    row("bb::search(\"outliner\", &query);",
        [] { bb::search("outliner", &g.query); });
    row("const char* items[] = {\"Edit\",\"Paint\",\"Animate\",\"Display\"};\n"
        "bb::combo(\"mode\", &current, items, 4);",
        [] { static const char* items[] = {"Edit", "Paint", "Animate", "Display"};
             bb::combo("##mode", &g.combo, items, 4); });
    row("const char* seg[] = {\"Object\",\"Edge\",\"Face\"};\n"
        "bb::segmented(\"select\", &mode, seg, 3);",
        [] { static const char* seg[] = {"Object", "Edge", "Face"};
             bb::segmented("select", &g.segment, seg, 3); });
    row("bb::checkbox(\"Visible\", &v);\nbb::checkbox(\"Locked\", &v);",
        [] { bb::checkbox("Visible", &g.check_a); bb::checkbox("Locked", &g.check_b); });
    row("bb::radio(\"Local\", &space, 0);\nbb::radio(\"Global\", &space, 1);",
        [] { bb::radio("Local", &g.radio, 0); ImGui::SameLine();
             bb::radio("Global", &g.radio, 1); });
    row("bb::color_edit(\"tint\", rgba);",
        [] { bb::color_edit("##tint", g.color); });

    // -- Feedback --------------------------------------------------
    sect("Feedback");
    row("bb::info(\"Model saved to project.\");",
        [] { bb::info("Model saved to project."); });
    row("bb::warning(\"3 faces have no UV mapping.\");",
        [] { bb::warning("3 faces have no UV mapping."); });
    row("bb::progress(0.62f, \"62%\");",
        [] { bb::progress(g.progress, "62%"); });
    row("bb::spinner();", [] { bb::spinner(); });
    row("bb::kbd(\"Ctrl\"); bb::kbd(\"Z\");",
        [] { bb::kbd("Ctrl"); ImGui::SameLine(0, 4); bb::kbd("Z"); });

    // -- Structure ------------------------------------------------
    sect("Structure");
    row("if (bb::collapsing(\"Transform\")) { ... }",
        [] { if (bb::collapsing("Transform")) {
                 ImGui::Indent(8);
                 bb::field_label("Pivot point");
                 ImGui::Unindent(8);
             } });

    ImGui::EndTable();
    ImGui::PopStyleVar(2);
}

} // namespace gallery
