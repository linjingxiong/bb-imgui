#include "shell.h"

#include "fonts.h"
#include "icons.h"
#include "logo.h"
#include "theme.h"

#include "imgui_internal.h" // DockBuilder / DockBuilderGetNode

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <cstdio>
#include <string>

namespace shell {

namespace {

bool  g_first_run = false;
bool  g_dragging = false;
double g_grab_x = 0, g_grab_y = 0;

const ProjectTab* g_tabs = nullptr;
int                g_tab_count = 0;
int                g_tab_active = 0;

Mode g_mode = Mode::Edit;
Chrome g_chrome = Chrome::Full;
ImVec2 g_content_pos(0, 0), g_content_size(0, 0);

const char*    g_status_left = "";
const char*    g_status_right = "";
const char*    g_status_tab = "";

const menu::Menu* g_menus = nullptr;
int               g_menu_count = 0;
const char*       g_menu_clicked = nullptr;

// Toolbar-row cursor state, live only between toolbar_begin()/toolbar_end().
float g_toolbar_x = 0.0f;
float g_toolbar_y = 0.0f;

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, 1.0f);
}

bool is_maximized(GLFWwindow* w) {
    return glfwGetWindowAttrib(w, GLFW_MAXIMIZED) != 0;
}

void toggle_maximize(GLFWwindow* w) {
    if (is_maximized(w))
        glfwRestoreWindow(w);
    else
        glfwMaximizeWindow(w);
}

inline constexpr float WBTN_W = 42.0f; // Blockbench #windows_window_menu li width

// A hand-drawn window-control button, Windows-style glyphs on a 16px grid.
// `kind`: 0 min, 1 maximize, 2 restore, 3 close.
bool window_button(const char* id, int kind, GLFWwindow* win) {
    const theme::Palette& p = theme::palette();
    ImVec2 size(WBTN_W, TITLEBAR_H);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        ImU32 bg = kind == 3 ? IM_COL32(232, 63, 66, 255) : u32(p.selected);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg);
    }
    ImU32 fg = u32(hovered ? p.light : p.text);
    ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const float r = 5.5f; // half-extent of an ~11px glyph (BB's is 16px viewBox)
    switch (kind) {
        case 0: // minimize — a thin horizontal bar
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, 1.0f);
            break;
        case 1: // maximize — hollow square
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 0.0f, 0, 1.0f);
            break;
        case 2: { // restore — two offset squares
            float d = 2.5f;
            dl->AddRect(ImVec2(c.x - r + d, c.y - r), ImVec2(c.x + r, c.y + r - d), fg, 0, 0, 1.0f);
            dl->AddRectFilled(ImVec2(c.x - r, c.y - r + d), ImVec2(c.x + r - d, c.y + r),
                              hovered ? u32(p.selected) : u32(p.frame));
            dl->AddRect(ImVec2(c.x - r, c.y - r + d), ImVec2(c.x + r - d, c.y + r), fg, 0, 0, 1.0f);
            break;
        }
        case 3: // close — a thin X
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 1.1f);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), fg, 1.1f);
            break;
    }
    (void)win;
    return clicked;
}

void titlebar(GLFWwindow* win) {
    const theme::Palette& p = theme::palette();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 tl = vp->Pos;
    ImVec2 br = ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + TITLEBAR_H);
    dl->AddRectFilled(tl, br, u32(p.frame)); // Blockbench header uses --color-frame

    ImGui::SetCursorScreenPos(ImVec2(tl.x, tl.y));

    // --- left: Blockbench wordmark + menu points --------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 0));

    float wordmark_w = 0.0f;
    if (logo::texture()) {
        ImVec2 ps = logo::pixel_size();
        wordmark_w = 134.0f;                       // Blockbench #corner_logo img width
        float draw_h = wordmark_w * (ps.y / ps.x); // keep aspect (~22px)
        float top = tl.y + (TITLEBAR_H - draw_h) * 0.5f;
        dl->AddImage(logo::texture(), ImVec2(tl.x + 10, top),
                     ImVec2(tl.x + 10 + wordmark_w, top + draw_h));
    } else {
        ImGui::PushFont(fonts::medium(), theme::size::WORDMARK);
        ImVec2 ts = ImGui::CalcTextSize("Blockbench");
        dl->AddText(ImVec2(tl.x + 12, tl.y + (TITLEBAR_H - ts.y) * 0.5f), u32(p.light),
                    "Blockbench");
        wordmark_w = ts.x;
        ImGui::PopFont();
    }

    float x = tl.x + 10 + wordmark_w + 16;
    g_menu_clicked = nullptr;
    ImGui::PushFont(fonts::medium(), theme::size::MENU_POINT);
    for (int i = 0; i < g_menu_count; i++) {
        const menu::Menu& m = g_menus[i];
        float bw = ImMax(ImGui::CalcTextSize(m.name).x + 16.0f, 42.0f); // BB: pad 8, min-w 42
        if (const char* hit = menu::point(m, i, ImVec2(x, tl.y), ImVec2(bw, TITLEBAR_H)))
            g_menu_clicked = hit;
        x += bw;
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(2);

    // --- right: window controls -----------------------------------------
    float bx = br.x - WBTN_W * 3;
    ImGui::SetCursorScreenPos(ImVec2(bx, tl.y));
    if (window_button("##min", 0, win))
        glfwIconifyWindow(win);
    ImGui::SetCursorScreenPos(ImVec2(bx + WBTN_W, tl.y));
    if (window_button("##max", is_maximized(win) ? 2 : 1, win))
        toggle_maximize(win);
    ImGui::SetCursorScreenPos(ImVec2(bx + WBTN_W * 2, tl.y));
    if (window_button("##close", 3, win))
        glfwSetWindowShouldClose(win, GLFW_TRUE);

    // --- middle: draggable free area ------------------------------------
    ImVec2 free_pos(x, tl.y);
    ImVec2 free_size(bx - x, TITLEBAR_H);
    if (free_size.x > 0) {
        ImGui::SetCursorScreenPos(free_pos);
        ImGui::InvisibleButton("##titledrag", free_size);
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            toggle_maximize(win);
            g_dragging = false;
        } else if (ImGui::IsItemActive() &&
                   ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            if (!g_dragging) {
                g_dragging = true;
                glfwGetCursorPos(win, &g_grab_x, &g_grab_y);
            }
            if (!is_maximized(win)) {
                double cx, cy;
                glfwGetCursorPos(win, &cx, &cy);
                int wx, wy;
                glfwGetWindowPos(win, &wx, &wy);
                glfwSetWindowPos(win, wx + (int)(cx - g_grab_x),
                                 wy + (int)(cy - g_grab_y));
            }
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        g_dragging = false;
}

// Blockbench #tab_bar: one .project_tab per open model, plus "+" (new tab)
// and a grid icon (recent files) at the end. Real measurements from
// window.css: height 34, bg --color-frame, padding-left 4; .project_tab
// width 240, padding 6px, margin-left 2; .selected gets --color-ui bg and
// rounded top corners (8px) so it visually merges into the panel below.
void tab_bar() {
    const theme::Palette& p = theme::palette();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 tl(vp->Pos.x, vp->Pos.y + TITLEBAR_H);
    ImVec2 br(vp->Pos.x + vp->Size.x, tl.y + TAB_BAR_H);
    dl->AddRectFilled(tl, br, u32(p.frame));

    const float tab_w = 240.0f, gap = 2.0f;
    float x = tl.x + 4.0f;
    ImGui::PushFont(nullptr, theme::size::SMALL);
    for (int i = 0; i < g_tab_count; i++) {
        const ProjectTab& t = g_tabs[i];
        bool active = (i == g_tab_active);
        ImVec2 pos(x, tl.y);
        ImVec2 size(tab_w, TAB_BAR_H);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(i);
        ImGui::InvisibleButton("tab", size);
        bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked())
            g_tab_active = i;
        ImGui::PopID();

        if (active)
            dl->AddRectFilled(pos, ImVec2(pos.x + tab_w, pos.y + TAB_BAR_H), u32(p.ui), 8.0f,
                              ImDrawFlags_RoundCornersTop);
        else if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + tab_w, pos.y + TAB_BAR_H),
                              u32(mix(p.frame, p.light, 0.06f)), 8.0f, ImDrawFlags_RoundCornersTop);

        ImVec2 ts = ImGui::CalcTextSize(t.name);
        float tx = pos.x + 10.0f;
        float ty = pos.y + (TAB_BAR_H - ts.y) * 0.5f;
        dl->AddText(ImVec2(tx, ty), u32(active ? p.light : p.text), t.name);

        // Unsaved-changes dot (Blockbench: replaced by an "x" close button
        // on hover; the dot-only state is enough for a static shell).
        if (t.modified && !hovered) {
            ImVec2 dp(pos.x + tab_w - 16.0f, pos.y + TAB_BAR_H * 0.5f);
            dl->AddCircleFilled(dp, 3.0f, u32(p.subtle_text));
        } else if (hovered) {
            ImVec2 xc(pos.x + tab_w - 16.0f, pos.y + TAB_BAR_H * 0.5f);
            dl->AddLine(ImVec2(xc.x - 4, xc.y - 4), ImVec2(xc.x + 4, xc.y + 4), u32(p.subtle_text));
            dl->AddLine(ImVec2(xc.x - 4, xc.y + 4), ImVec2(xc.x + 4, xc.y - 4), u32(p.subtle_text));
        }
        x += tab_w + gap;
    }
    ImGui::PopFont();

    // "+" new-tab button.
    {
        ImVec2 pos(x + 4.0f, tl.y);
        ImVec2 size(TAB_BAR_H, TAB_BAR_H);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##newtab", size);
        bool hovered = ImGui::IsItemHovered();
        ImU32 fg = u32(hovered ? p.light : p.subtle_text);
        ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
        dl->AddLine(ImVec2(c.x - 6, c.y), ImVec2(c.x + 6, c.y), fg, 1.3f);
        dl->AddLine(ImVec2(c.x, c.y - 6), ImVec2(c.x, c.y + 6), fg, 1.3f);
    }

    // Grid icon (recent files) at the far right of the tab bar.
    {
        ImVec2 pos(br.x - TAB_BAR_H, tl.y);
        ImVec2 size(TAB_BAR_H, TAB_BAR_H);
        ImGui::SetCursorScreenPos(pos);
        ImGui::InvisibleButton("##recent", size);
        bool hovered = ImGui::IsItemHovered();
        ImGui::PushFont(fonts::body(), 16.0f);
        ImVec2 ts = ImGui::CalcTextSize(ICON_VIEW_LIST);
        dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                    u32(hovered ? p.light : p.subtle_text), ICON_VIEW_LIST);
        ImGui::PopFont();
    }
}

// Blockbench .tool: 36x30, active gets an accent-filled 4px-radius pill.
bool tool_button_impl(const char* icon_glyph, bool active) {
    const theme::Palette& p = theme::palette();
    ImVec2 size(36.0f, TOOLBAR_H);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::PushID(icon_glyph);
    ImGui::InvisibleButton("tool", size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();
    ImGui::PopID();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (active)
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), u32(p.accent), 4.0f);
    else if (hovered)
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), u32(p.selected), 4.0f);

    ImGui::PushFont(fonts::body(), 17.0f);
    ImVec2 ts = ImGui::CalcTextSize(icon_glyph);
    dl->AddText(ImVec2(pos.x + (size.x - ts.x) * 0.5f, pos.y + (size.y - ts.y) * 0.5f),
                u32(active ? p.accent_text : (hovered ? p.light : p.text)), icon_glyph);
    ImGui::PopFont();
    return clicked;
}

// Blockbench #mode_selector: right-aligned (margin-left: auto), each li is
// padding 2px 7px, radius 5, font-size 1.1em; .selected gets accent bg.
void mode_selector() {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    static const char* NAMES[] = {"Edit", "Paint", "Animate"};
    const int count = 3;

    ImGui::PushFont(nullptr, theme::size::SMALL * 1.1f);
    float total_w = 0.0f;
    float widths[count];
    for (int i = 0; i < count; i++) {
        widths[i] = ImGui::CalcTextSize(NAMES[i]).x + 14.0f; // padding 7px each side
        total_w += widths[i] + (i ? 6.0f : 0.0f);
    }

    ImGuiViewport* vp = ImGui::GetMainViewport();
    float right_edge = vp->Pos.x + vp->Size.x - 12.0f;
    float x = right_edge - total_w;
    float y = g_toolbar_y + (TOOLBAR_H - (ImGui::GetTextLineHeight() + 4.0f)) * 0.5f;
    float h = ImGui::GetTextLineHeight() + 4.0f;

    for (int i = 0; i < count; i++) {
        ImVec2 pos(x, y);
        ImGui::SetCursorScreenPos(pos);
        ImGui::PushID(i);
        ImGui::InvisibleButton("mode", ImVec2(widths[i], h));
        bool hovered = ImGui::IsItemHovered();
        bool sel = ((int)g_mode == i);
        if (ImGui::IsItemClicked()) g_mode = (Mode)i;
        ImGui::PopID();

        if (sel)
            dl->AddRectFilled(pos, ImVec2(pos.x + widths[i], pos.y + h), u32(p.accent), 5.0f);
        else if (hovered)
            dl->AddRectFilled(pos, ImVec2(pos.x + widths[i], pos.y + h), u32(p.selected), 5.0f);
        ImVec2 ts = ImGui::CalcTextSize(NAMES[i]);
        dl->AddText(ImVec2(pos.x + (widths[i] - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f),
                    u32(sel ? p.accent_text : (hovered ? p.light : p.text)), NAMES[i]);
        x += widths[i] + 6.0f;
    }
    ImGui::PopFont();
}

// Invisible drag zones along the window edges (borderless windows get no OS
// resize). 8 zones: edges + corners.
void resize_handles(GLFWwindow* win) {
    if (is_maximized(win))
        return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float B = 6.0f;
    const float W = vp->Size.x, H = vp->Size.y;
    const ImVec2 O = vp->Pos;

    struct Zone {
        ImVec2 pos, size;
        int dx, dy; // which edges move: -1 left/top, +1 right/bottom, 0 fixed
        ImGuiMouseCursor cursor;
    };
    const Zone zones[] = {
        {{O.x, O.y + B}, {B, H - 2 * B}, -1, 0, ImGuiMouseCursor_ResizeEW},
        {{O.x + W - B, O.y + B}, {B, H - 2 * B}, 1, 0, ImGuiMouseCursor_ResizeEW},
        {{O.x + B, O.y}, {W - 2 * B, B}, 0, -1, ImGuiMouseCursor_ResizeNS},
        {{O.x + B, O.y + H - B}, {W - 2 * B, B}, 0, 1, ImGuiMouseCursor_ResizeNS},
        {{O.x, O.y}, {B, B}, -1, -1, ImGuiMouseCursor_ResizeNWSE},
        {{O.x + W - B, O.y + H - B}, {B, B}, 1, 1, ImGuiMouseCursor_ResizeNWSE},
        {{O.x + W - B, O.y}, {B, B}, 1, -1, ImGuiMouseCursor_ResizeNESW},
        {{O.x, O.y + H - B}, {B, B}, -1, 1, ImGuiMouseCursor_ResizeNESW},
    };

    static int active = -1;
    static double grab_x = 0, grab_y = 0;
    static int start_x = 0, start_y = 0, start_w = 0, start_h = 0;

    for (int i = 0; i < 8; i++) {
        const Zone& z = zones[i];
        ImGui::SetCursorScreenPos(z.pos);
        ImGui::InvisibleButton(("##rz" + std::to_string(i)).c_str(), z.size);
        if (ImGui::IsItemHovered() || (active == i))
            ImGui::SetMouseCursor(z.cursor);
        if (ImGui::IsItemActivated()) {
            active = i;
            glfwGetCursorPos(win, &grab_x, &grab_y);
            glfwGetWindowPos(win, &start_x, &start_y);
            glfwGetWindowSize(win, &start_w, &start_h);
        }
        if (active == i && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            double cx, cy;
            glfwGetCursorPos(win, &cx, &cy);
            // cursor pos is window-relative; convert drag to screen delta
            int wx, wy;
            glfwGetWindowPos(win, &wx, &wy);
            double sx = wx + cx, sy = wy + cy;
            double dx = sx - (start_x + grab_x);
            double dy = sy - (start_y + grab_y);

            int nx = start_x, ny = start_y, nw = start_w, nh = start_h;
            const int MINW = 640, MINH = 400;
            if (z.dx < 0) { nx = start_x + (int)dx; nw = start_w - (int)dx; }
            if (z.dx > 0) { nw = start_w + (int)dx; }
            if (z.dy < 0) { ny = start_y + (int)dy; nh = start_h - (int)dy; }
            if (z.dy > 0) { nh = start_h + (int)dy; }
            if (nw < MINW) { if (z.dx < 0) nx -= (MINW - nw); nw = MINW; }
            if (nh < MINH) { if (z.dy < 0) ny -= (MINH - nh); nh = MINH; }
            glfwSetWindowPos(win, nx, ny);
            glfwSetWindowSize(win, nw, nh);
        }
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
        active = -1;
}

void status_bar() {
    const theme::Palette& p = theme::palette();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 tl(vp->Pos.x, vp->Pos.y + vp->Size.y - STATUS_H);
    ImVec2 br(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y);
    // Blockbench's #status_bar uses --color-ui, not a darker "back" tone.
    dl->AddRectFilled(tl, br, u32(p.ui));
    dl->AddLine(ImVec2(tl.x, tl.y + 0.5f), ImVec2(br.x, tl.y + 0.5f), u32(p.border), 1.0f);

    ImGui::PushFont(nullptr, theme::size::SMALL);
    float ty = tl.y + (STATUS_H - ImGui::GetTextLineHeight()) * 0.5f;
    if (g_status_left && g_status_left[0])
        dl->AddText(ImVec2(tl.x + 12, ty), u32(p.subtle_text), g_status_left);

    float rx = br.x - 12.0f;
    if (g_status_right && g_status_right[0]) {
        ImVec2 ts = ImGui::CalcTextSize(g_status_right);
        rx -= ts.x;
        dl->AddText(ImVec2(rx, ty), u32(p.subtle_text), g_status_right);
        rx -= 16.0f;
    }
    // Bottom-right "Collections"-style tab: a small bordered pill, distinct
    // from the plain status text either side of it.
    if (g_status_tab && g_status_tab[0]) {
        ImVec2 ts = ImGui::CalcTextSize(g_status_tab);
        float tab_w = ts.x + 16.0f, tab_h = STATUS_H - 8.0f;
        rx -= tab_w;
        ImVec2 tp(rx, tl.y + 4.0f);
        dl->AddRectFilled(tp, ImVec2(tp.x + tab_w, tp.y + tab_h), u32(p.button), 3.0f);
        dl->AddText(ImVec2(tp.x + 8.0f, tp.y + (tab_h - ts.y) * 0.5f), u32(p.text),
                    g_status_tab);
    }
    ImGui::PopFont();
}

} // namespace

// Default panel layout, built with DockBuilder the first time. Panel windows
// are Begin()'d by the caller under these exact names.
void build_default_layout(ImGuiID dock_id, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dock_id);
    ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock_id, size);

    ImGuiID center = dock_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.24f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.32f, nullptr, &center);

    for (ImGuiID id : {left, right, center})
        if (ImGuiDockNode* n = ImGui::DockBuilderGetNode(id))
            n->LocalFlags |= ImGuiDockNodeFlags_NoTabBar;

    ImGui::DockBuilderDockWindow("Left", left);
    ImGui::DockBuilderDockWindow("Right", right);
    ImGui::DockBuilderDockWindow("Workspace", center);
    ImGui::DockBuilderFinish(dock_id);
}

ImGuiID begin(GLFWwindow* window) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                             ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoBringToFrontOnFocus |
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##host", nullptr, flags);
    ImGui::PopStyleVar(3);

    resize_handles(window);
    titlebar(window);

    if (g_chrome == Chrome::Minimal) {
        g_first_run = false;
        g_content_pos = ImVec2(vp->Pos.x, vp->Pos.y + TITLEBAR_H);
        g_content_size = ImVec2(vp->Size.x, vp->Size.y - TITLEBAR_H);
        ImGui::SetCursorScreenPos(g_content_pos);
        return 0;
    }

    tab_bar();
    status_bar();

    ImGuiID dock_id = ImGui::GetID("##dockspace");
    const float top = TITLEBAR_H + TAB_BAR_H + TOOLBAR_H;
    const ImVec2 dock_size(vp->Size.x, vp->Size.y - top - STATUS_H);

    g_first_run = (ImGui::DockBuilderGetNode(dock_id) == nullptr);
    if (g_first_run)
        build_default_layout(dock_id, dock_size);

    ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x, vp->Pos.y + top));
    ImGui::DockSpace(dock_id, dock_size, ImGuiDockNodeFlags_None);
    g_content_pos = ImVec2(vp->Pos.x, vp->Pos.y + top);
    g_content_size = dock_size;
    return dock_id;
}

void set_chrome(Chrome c) { g_chrome = c; }
Chrome chrome() { return g_chrome; }
void content_rect(ImVec2* pos, ImVec2* size) {
    if (pos) *pos = g_content_pos;
    if (size) *size = g_content_size;
}

void end() { ImGui::End(); }

bool first_run() { return g_first_run; }

void set_tabs(const ProjectTab* tabs, int count) {
    g_tabs = tabs;
    g_tab_count = count;
    if (g_tab_active >= count) g_tab_active = 0;
}
int  tabs_active() { return g_tab_active; }
void set_tabs_active(int index) {
    if (index >= 0 && index < g_tab_count) g_tab_active = index;
}

void set_mode(Mode m) { g_mode = m; }
Mode mode() { return g_mode; }

void toolbar_begin(const char* panel_label) {
    const theme::Palette& p = theme::palette();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    ImVec2 tl(vp->Pos.x, vp->Pos.y + TITLEBAR_H + TAB_BAR_H);
    ImVec2 br(vp->Pos.x + vp->Size.x, tl.y + TOOLBAR_H);
    dl->AddRectFilled(tl, br, u32(p.ui));
    dl->AddLine(ImVec2(tl.x, br.y - 0.5f), ImVec2(br.x, br.y - 0.5f), u32(p.border), 1.0f);

    g_toolbar_y = tl.y;
    g_toolbar_x = tl.x + 10.0f;

    if (panel_label && panel_label[0]) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImVec2 ts = ImGui::CalcTextSize(panel_label);
        dl->AddText(ImVec2(g_toolbar_x, tl.y + (TOOLBAR_H - ts.y) * 0.5f), u32(p.text),
                    panel_label);
        ImGui::PopFont();
        g_toolbar_x += ts.x + 16.0f;
    }
    mode_selector();
    ImGui::SetCursorScreenPos(ImVec2(g_toolbar_x, g_toolbar_y));
}

bool tool_button(const char* icon_glyph, bool active) {
    ImGui::SetCursorScreenPos(ImVec2(g_toolbar_x, g_toolbar_y));
    bool clicked = tool_button_impl(icon_glyph, active);
    g_toolbar_x += 36.0f + 2.0f;
    return clicked;
}

void toolbar_end() {}

void set_status(const char* left, const char* right) {
    g_status_left = left ? left : "";
    g_status_right = right ? right : "";
}
void set_status_tab(const char* label) { g_status_tab = label ? label : ""; }
void set_menus(const menu::Menu* menus, int count) {
    g_menus = menus;
    g_menu_count = count;
}
const char* menu_clicked() { return g_menu_clicked; }

} // namespace shell
