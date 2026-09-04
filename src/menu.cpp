#include "menu.h"

#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include <algorithm>
#include <cstdio> // snprintf

namespace menu {
namespace {

int g_open = -1;        // index of the open menu point, or -1
int g_open_frame = -1;  // frame the current menu opened

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

constexpr float ROW_H = 30.0f;
constexpr float SEP_H = 8.0f;
constexpr float ICON_COL = 34.0f;
constexpr float PAD_R = 12.0f;

float measure_width(const Menu& m) {
    float w = 190.0f;
    ImGui::PushFont(nullptr, theme::size::MENU_ITEM);
    for (int i = 0; i < m.count; i++) {
        const Item& it = m.items[i];
        if (!it.label)
            continue;
        float row = ICON_COL + ImGui::CalcTextSize(it.label).x + 24.0f;
        if (it.shortcut)
            row += ImGui::CalcTextSize(it.shortcut).x + 24.0f;
        if (it.has_submenu)
            row += 20.0f;
        w = std::max(w, row);
    }
    ImGui::PopFont();
    return std::min(w, 380.0f);
}

// Returns activated leaf label or nullptr.
const char* draw_rows(const Menu& m, float w) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const char* result = nullptr;

    for (int i = 0; i < m.count; i++) {
        const Item& it = m.items[i];
        ImVec2 pos = ImGui::GetCursorScreenPos();

        if (!it.label) { // separator
            ImGui::Dummy(ImVec2(w, SEP_H));
            dl->AddLine(ImVec2(pos.x + 8, pos.y + SEP_H * 0.5f),
                        ImVec2(pos.x + w - 8, pos.y + SEP_H * 0.5f),
                        u32(mix(p.bright_ui, ImVec4(0, 0, 0, 1), 0.16f)), 1.0f);
            continue;
        }

        char id[24];
        std::snprintf(id, sizeof(id), "##mi%d", i);
        ImGui::InvisibleButton(id, ImVec2(w, ROW_H));
        bool hot = it.enabled && ImGui::IsItemHovered();
        if (it.enabled && !it.has_submenu && ImGui::IsItemClicked())
            result = it.label;

        ImVec2 br(pos.x + w, pos.y + ROW_H);
        dl->AddRectFilled(pos, br, u32(hot ? p.accent : p.bright_ui));
        if (it.checked)
            dl->AddRectFilled(pos, ImVec2(pos.x + 4, br.y),
                              u32(hot ? p.accent_text : p.accent));

        ImVec4 fgv = hot ? p.accent_text : p.bright_ui_text;
        if (!it.enabled)
            fgv = mix(p.bright_ui, p.bright_ui_text, 0.4f);
        ImU32 fg = u32(fgv);
        float cy = pos.y + ROW_H * 0.5f;

        if (it.icon) {
            ImGui::PushFont(fonts::body(), 20.0f);
            ImVec2 ts = ImGui::CalcTextSize(it.icon);
            dl->AddText(ImVec2(pos.x + 8 + (18 - ts.x) * 0.5f, cy - ts.y * 0.5f), fg,
                        it.icon);
            ImGui::PopFont();
        }
        ImGui::PushFont(nullptr, theme::size::MENU_ITEM);
        ImVec2 lts = ImGui::CalcTextSize(it.label);
        dl->AddText(ImVec2(pos.x + ICON_COL, cy - lts.y * 0.5f), fg, it.label);
        ImGui::PopFont();

        if (it.has_submenu) {
            ImGui::PushFont(fonts::body(), 18.0f);
            ImVec2 ts = ImGui::CalcTextSize(ICON_NAVIGATE_NEXT);
            dl->AddText(ImVec2(br.x - PAD_R - ts.x, cy - ts.y * 0.5f), fg,
                        ICON_NAVIGATE_NEXT);
            ImGui::PopFont();
        } else if (it.shortcut) {
            ImGui::PushFont(nullptr, theme::size::SMALL);
            ImVec2 ts = ImGui::CalcTextSize(it.shortcut);
            ImU32 sc = hot ? fg : u32(mix(p.bright_ui, p.bright_ui_text, 0.55f));
            dl->AddText(ImVec2(br.x - PAD_R - ts.x, cy - ts.y * 0.5f), sc, it.shortcut);
            ImGui::PopFont();
        }
    }
    return result;
}

} // namespace

const char* point(const Menu& m, int index, ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImGui::SetCursorScreenPos(pos);
    char bid[24];
    std::snprintf(bid, sizeof(bid), "##mbp%d", index);
    ImGui::InvisibleButton(bid, size);
    bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    if (ImGui::IsItemClicked()) {
        g_open = (g_open == index) ? -1 : index;
        g_open_frame = ImGui::GetFrameCount();
    } else if (g_open != -1 && g_open != index && hovered) {
        g_open = index;
        g_open_frame = ImGui::GetFrameCount();
    }

    bool active = (g_open == index);
    if (active || hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y),
                          u32(active ? p.accent : p.ui));

    // Blockbench's CSS is Regular, but Chrome/DirectWrite renders it heavier than
    // ImGui's stb rasterizer; SemiBold + bright white matches the look.
    ImGui::PushFont(fonts::medium(), theme::size::MENU_POINT);
    ImVec2 ts = ImGui::CalcTextSize(m.name);
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                u32(active ? p.accent_text : p.light), m.name);
    ImGui::PopFont();

    const char* result = nullptr;
    if (!active)
        return result;

    // --- dropdown (own borderless window, drawn on top) -------------------
    float w = measure_width(m);
    ImGui::SetNextWindowPos(ImVec2(pos.x, pos.y + size.y));
    ImGui::SetNextWindowSize(ImVec2(w, 0));
    if (ImGui::GetFrameCount() == g_open_frame + 1)
        ImGui::SetNextWindowFocus();

    ImGui::PushStyleColor(ImGuiCol_WindowBg, p.bright_ui);
    ImGui::PushStyleColor(ImGuiCol_Border, mix(p.bright_ui, ImVec4(0, 0, 0, 1), 0.18f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 6));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));

    char wid[24];
    std::snprintf(wid, sizeof(wid), "##menuwin%d", index);
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoScrollWithMouse |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoCollapse;
    bool win_hovered = false;
    if (ImGui::Begin(wid, nullptr, flags)) {
        result = draw_rows(m, w);
        win_hovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    }
    ImGui::End();

    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);

    // Dismiss on a click that missed both this bar point and the dropdown.
    if (result) {
        g_open = -1;
    } else if (ImGui::GetFrameCount() > g_open_frame &&
               ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hovered && !win_hovered) {
        g_open = -1;
    }
    return result;
}

bool any_open() { return g_open != -1; }

} // namespace menu
