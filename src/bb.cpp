#include "bb.h"

#include "fonts.h"
#include "theme.h"

#include <cctype>
#include <string>

namespace bb {
namespace {

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

std::string upper(const char* s) {
    std::string out(s);
    for (char& c : out) c = (char)std::toupper((unsigned char)c);
    return out;
}

} // namespace

void panel_header(const char* title) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 br(pos.x + w, pos.y + PANEL_HEADER_H);

    dl->AddRectFilled(pos, br, u32(p.back));
    dl->AddLine(ImVec2(pos.x, br.y - 0.5f), ImVec2(br.x, br.y - 0.5f), u32(p.border), 1.0f);

    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    std::string label = upper(title);
    ImVec2 ts = ImGui::CalcTextSize(label.c_str());
    dl->AddText(ImVec2(pos.x + 12, pos.y + (PANEL_HEADER_H - ts.y) * 0.5f),
                u32(p.subtle_text), label.c_str());
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(w, PANEL_HEADER_H));
}

bool begin_panel(const char* name) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin(name, nullptr,
                             ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove);
    ImGui::PopStyleVar();

    panel_header(name);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    ImGui::Indent(10.0f);
    ImGui::Dummy(ImVec2(0, 4));
    return open;
}

void end_panel() {
    ImGui::Unindent(10.0f);
    ImGui::PopStyleVar();
    ImGui::End();
}

} // namespace bb
