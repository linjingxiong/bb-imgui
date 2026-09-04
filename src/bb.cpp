#include "bb.h"

#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace bb {
namespace {

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

const theme::Palette& pal() { return theme::palette(); }

std::string upper(const char* s) {
    std::string out(s);
    for (char& c : out) c = (char)std::toupper((unsigned char)c);
    return out;
}

void fmt_num(char* buf, int n, double v, int decimals) {
    std::snprintf(buf, n, "%.*f", decimals, v);
    // trim a trailing ".00" style when the value is integral-ish
    if (decimals > 0) {
        char* dot = std::strchr(buf, '.');
        if (dot) {
            char* end = buf + std::strlen(buf) - 1;
            while (end > dot && *end == '0') *end-- = 0;
            if (end == dot) *end = 0;
        }
    }
}

} // namespace

// ===========================================================================
// Panel chrome
// ===========================================================================
void panel_header(const char* title) {
    const theme::Palette& p = pal();
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

void field_label(const char* text) {
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::TextColored(pal().subtle_text, "%s", text);
    ImGui::PopFont();
}

// ===========================================================================
// Buttons
// ===========================================================================
bool button(const char* label, ImVec2 size) { return ImGui::Button(label, size); }

bool primary_button(const char* label, ImVec2 size) {
    const theme::Palette& p = pal();
    ImGui::PushStyleColor(ImGuiCol_Button, p.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, mix(p.accent, p.light, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, mix(p.accent, ImVec4(0, 0, 0, 1), 0.1f));
    ImGui::PushStyleColor(ImGuiCol_Text, p.accent_text);
    bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool icon_button(const char* icon_glyph, bool active) {
    const theme::Palette& p = pal();
    ImVec2 size(30, 28);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(icon_glyph, size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 br(pos.x + size.x, pos.y + size.y);
    if (hovered || active)
        dl->AddRectFilled(pos, br, u32(active ? mix(p.ui, p.accent, 0.15f) : p.button),
                          theme::RADIUS);
    if (active)
        dl->AddRectFilled(ImVec2(pos.x + 6, br.y - 2), ImVec2(br.x - 6, br.y), u32(p.accent));
    ImGui::PushFont(fonts::body(), 17.0f);
    ImVec2 ts = ImGui::CalcTextSize(icon_glyph);
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                u32(active ? p.accent : (hovered ? p.light : p.text)), icon_glyph);
    ImGui::PopFont();
    return clicked;
}

bool toggle(const char* label, bool* v) {
    const theme::Palette& p = pal();
    ImGui::PushID(label);
    float h = 16.0f, w = 28.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("t", ImVec2(w, h));
    bool changed = false;
    if (ImGui::IsItemClicked()) { *v = !*v; changed = true; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 track = u32(*v ? p.accent : p.deep);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), track, h * 0.5f);
    float kx = *v ? pos.x + w - h * 0.5f : pos.x + h * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, pos.y + h * 0.5f), h * 0.5f - 3.0f,
                        u32(*v ? p.accent_text : p.light));

    if (label[0] != '#') {
        ImGui::SameLine(0, 8);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
    }
    ImGui::PopID();
    return changed;
}

// ===========================================================================
// Numeric
// ===========================================================================
bool num_slider(const char* id, double* v, const NumOpts& o) {
    const theme::Palette& p = pal();
    ImGui::PushID(id);
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID edit_id = ImGui::GetID("edit");
    bool editing = st->GetBool(edit_id, false);

    float w = o.width > 0 ? o.width : ImGui::GetContentRegionAvail().x;
    float h = 30.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    bool changed = false;

    if (editing) {
        ImGui::SetNextItemWidth(w);
        double before = *v;
        ImGui::SetKeyboardFocusHere();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.*f", o.decimals, *v);
        if (ImGui::InputText("##e", buf, sizeof(buf),
                             ImGuiInputTextFlags_CharsDecimal |
                                 ImGuiInputTextFlags_EnterReturnsTrue |
                                 ImGuiInputTextFlags_AutoSelectAll)) {
            *v = std::atof(buf);
        }
        if (!ImGui::IsItemActive()) {
            *v = std::atof(buf);
            st->SetBool(edit_id, false);
        }
        changed = (*v != before);
    } else {
        ImGui::InvisibleButton("s", ImVec2(w, h));
        bool hovered = ImGui::IsItemHovered();
        bool active = ImGui::IsItemActive();
        if (hovered)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        if (active && ImGui::GetIO().MouseDelta.x != 0.0f) {
            *v += ImGui::GetIO().MouseDelta.x * o.step;
            changed = true;
        }
        if (hovered && ImGui::GetIO().MouseWheel != 0.0f) {
            *v += ImGui::GetIO().MouseWheel * o.step;
            changed = true;
        }
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && hovered)
            st->SetBool(edit_id, true);
        if (o.min != o.max)
            *v = std::clamp(*v, o.min, o.max);

        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 br(pos.x + w, pos.y + h);
        dl->AddRectFilled(pos, br, u32(p.deep), theme::RADIUS);
        dl->AddRect(pos, br, u32(hovered ? p.selected : p.border), theme::RADIUS);

        char buf[64];
        fmt_num(buf, sizeof(buf), *v, o.decimals);
        std::string text = buf;
        if (o.suffix) text += o.suffix;
        ImVec2 ts = ImGui::CalcTextSize(text.c_str());
        dl->AddText(ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                    u32(hovered ? p.light : p.text), text.c_str());
    }
    ImGui::PopID();
    return changed;
}

bool vec3(const char* id, float v[3]) {
    static const char* AX[3] = {"X", "Y", "Z"};
    ImGui::PushID(id);
    bool changed = false;
    float avail = ImGui::GetContentRegionAvail().x;
    float cell = (avail - 16.0f) / 3.0f;
    for (int i = 0; i < 3; i++) {
        if (i) ImGui::SameLine(0, 8);
        ImGui::BeginGroup();
        field_label(AX[i]);
        double d = v[i];
        NumOpts o;
        o.width = cell;
        o.step = 0.1;
        if (num_slider(AX[i], &d, o)) { v[i] = (float)d; changed = true; }
        ImGui::EndGroup();
    }
    ImGui::PopID();
    return changed;
}

// ===========================================================================
// Inputs
// ===========================================================================
bool input_text(const char* id, std::string* s, const char* hint) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (hint)
        return ImGui::InputTextWithHint(id, hint, s);
    return ImGui::InputText(id, s);
}

bool search(const char* id, std::string* s) {
    const theme::Palette& p = pal();
    ImGui::PushID(id);
    float w = ImGui::GetContentRegionAvail().x;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(
        fonts::body(), 16.0f, ImVec2(pos.x + 8, pos.y + 5), u32(p.subtle_text), ICON_SEARCH);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + 28, pos.y));
    ImGui::SetNextItemWidth(w - 28);
    bool r = ImGui::InputTextWithHint("##s", "Search\xe2\x80\xa6", s);
    ImGui::PopID();
    return r;
}

bool combo(const char* id, int* current, const char* const items[], int count) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    return ImGui::Combo(id, current, items, count);
}

bool checkbox(const char* label, bool* v) { return ImGui::Checkbox(label, v); }

bool radio(const char* label, int* current, int value) {
    return ImGui::RadioButton(label, current, value);
}

bool segmented(const char* id, int* current, const char* const labels[], int count) {
    const theme::Palette& p = pal();
    ImGui::PushID(id);
    bool changed = false;
    float w = ImGui::GetContentRegionAvail().x;
    float cw = w / count;
    float h = 30.0f;
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), u32(p.deep), theme::RADIUS);
    for (int i = 0; i < count; i++) {
        ImVec2 cp(pos.x + i * cw, pos.y);
        ImGui::SetCursorScreenPos(cp);
        ImGui::PushID(i);
        ImGui::InvisibleButton("seg", ImVec2(cw, h));
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked() && *current != i) { *current = i; changed = true; }
        ImGui::PopID();
        bool on = (*current == i);
        if (on)
            dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + h), u32(p.accent), theme::RADIUS);
        else if (hovered)
            dl->AddRectFilled(cp, ImVec2(cp.x + cw, cp.y + h), u32(p.button), theme::RADIUS);
        ImVec2 ts = ImGui::CalcTextSize(labels[i]);
        dl->AddText(ImVec2(cp.x + (cw - ts.x) * 0.5f, cp.y + (h - ts.y) * 0.5f),
                    u32(on ? p.accent_text : (hovered ? p.light : p.text)), labels[i]);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
    ImGui::Dummy(ImVec2(w, 0));
    ImGui::PopID();
    return changed;
}

bool slider_float(const char* id, float* v, float mn, float mx, const char* fmt) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    return ImGui::SliderFloat(id, v, mn, mx, fmt);
}

bool color_edit(const char* id, float col[4], bool alpha) {
    ImGuiColorEditFlags f = ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_DisplayHex;
    if (!alpha) f |= ImGuiColorEditFlags_NoAlpha;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    return ImGui::ColorEdit4(id, col, f);
}

// ===========================================================================
// Feedback
// ===========================================================================
void info(const char* text) {
    const theme::Palette& p = pal();
    ImGui::PushFont(fonts::body(), 16.0f);
    ImGui::TextColored(p.accent, "%s", ICON_INFO);
    ImGui::PopFont();
    ImGui::SameLine(0, 6);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(p.text, "%s", text);
}

void warning(const char* text) {
    ImVec4 amber(0.98f, 0.75f, 0.29f, 1.0f);
    ImGui::PushFont(fonts::body(), 16.0f);
    ImGui::TextColored(amber, "%s", ICON_WARNING);
    ImGui::PopFont();
    ImGui::SameLine(0, 6);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(pal().text, "%s", text);
}

void progress(float frac, const char* overlay) {
    const theme::Palette& p = pal();
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, p.accent);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, p.deep);
    ImGui::ProgressBar(frac, ImVec2(-FLT_MIN, 14.0f), overlay ? overlay : "");
    ImGui::PopStyleColor(2);
}

void spinner(float radius) {
    const theme::Palette& p = pal();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 c(pos.x + radius, pos.y + radius);
    ImGui::Dummy(ImVec2(radius * 2, radius * 2));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float t = (float)ImGui::GetTime() * 6.0f;
    int seg = 20;
    for (int i = 0; i < seg; i++) {
        float a0 = t + (float)i / seg * 6.2831853f;
        float a1 = t + (float)(i + 1) / seg * 6.2831853f;
        float alpha = (float)i / seg;
        dl->PathArcTo(c, radius, a0, a1, 3);
        dl->PathStroke(u32(ImVec4(p.accent.x, p.accent.y, p.accent.z, alpha)), 0, 2.5f);
    }
}

void kbd(const char* text) {
    const theme::Palette& p = pal();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImVec2 ts = ImGui::CalcTextSize(text);
    ImVec2 pad(6, 3);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImVec2 sz(ts.x + pad.x * 2, ts.y + pad.y * 2);
    ImGui::Dummy(sz);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), u32(p.deep), 4.0f);
    dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), u32(p.border), 4.0f);
    dl->AddText(ImVec2(pos.x + pad.x, pos.y + pad.y), u32(p.subtle_text), text);
    ImGui::PopFont();
}

bool collapsing(const char* label, bool default_open) {
    ImGuiTreeNodeFlags f = ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
    if (default_open) f |= ImGuiTreeNodeFlags_DefaultOpen;
    const theme::Palette& p = pal();
    ImGui::PushStyleColor(ImGuiCol_Header, p.back);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, mix(p.back, p.light, 0.06f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, p.back);
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    bool open = ImGui::CollapsingHeader(label, f);
    ImGui::PopFont();
    ImGui::PopStyleColor(3);
    return open;
}

} // namespace bb
