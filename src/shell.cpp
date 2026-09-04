#include "shell.h"

#include "fonts.h"
#include "theme.h"

#include "imgui_internal.h" // DockBuilder / DockBuilderGetNode

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <cmath>
#include <string>

namespace shell {

const char* const MENU_POINTS[] = {"File",  "Edit", "Transform", "UV",
                                   "Tools", "View", "Help"};
const int MENU_POINT_COUNT = (int)(sizeof(MENU_POINTS) / sizeof(MENU_POINTS[0]));

namespace {

bool  g_first_run = false;
bool  g_dragging = false;
double g_grab_x = 0, g_grab_y = 0;

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

bool is_maximized(GLFWwindow* w) {
    return glfwGetWindowAttrib(w, GLFW_MAXIMIZED) != 0;
}

void toggle_maximize(GLFWwindow* w) {
    if (is_maximized(w))
        glfwRestoreWindow(w);
    else
        glfwMaximizeWindow(w);
}

// A hand-drawn window-control button (46 x TITLEBAR_H). `kind`: 0 min, 1 max,
// 2 restore, 3 close.
bool window_button(const char* id, int kind, GLFWwindow* win) {
    const theme::Palette& p = theme::palette();
    ImVec2 size(46.0f, TITLEBAR_H);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, size);
    bool hovered = ImGui::IsItemHovered();
    bool clicked = ImGui::IsItemClicked();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (hovered) {
        ImU32 bg = kind == 3 ? IM_COL32(232, 68, 68, 255) : u32(p.selected);
        dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg);
    }
    ImU32 fg = u32(hovered ? p.light : p.subtle_text);
    ImVec2 c(pos.x + size.x * 0.5f, pos.y + size.y * 0.5f);
    const float r = 5.0f;
    switch (kind) {
        case 0: // minimize
            dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), fg, 1.0f);
            break;
        case 1: // maximize
            dl->AddRect(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 0.0f, 0,
                        1.0f);
            break;
        case 2: // restore
            dl->AddRect(ImVec2(c.x - r + 2, c.y - r), ImVec2(c.x + r, c.y + r - 2), fg,
                        0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(c.x - r, c.y - r + 2), ImVec2(c.x + r - 2, c.y + r), fg,
                        0.0f, 0, 1.0f);
            break;
        case 3: // close
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), fg, 1.2f);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), fg, 1.2f);
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
    dl->AddRectFilled(tl, br, u32(p.back));
    dl->AddLine(ImVec2(tl.x, br.y - 0.5f), ImVec2(br.x, br.y - 0.5f), u32(p.border), 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(tl.x, tl.y));

    // --- left: wordmark + menu points -------------------------------------
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 0));

    ImGui::PushFont(fonts::medium(), theme::size::MENU_POINT);
    ImGui::PushStyleColor(ImGuiCol_Text, p.light);
    ImGui::AlignTextToFramePadding();
    ImGui::SetCursorScreenPos(ImVec2(tl.x + 12, tl.y + (TITLEBAR_H - ImGui::GetFontSize()) * 0.5f));
    ImGui::TextUnformatted("Blockbench");
    ImGui::PopStyleColor();
    ImGui::PopFont();

    float x = tl.x + 12 + ImGui::CalcTextSize("Blockbench").x + 18;
    ImGui::PushFont(nullptr, theme::size::MENU_POINT);
    for (int i = 0; i < MENU_POINT_COUNT; i++) {
        const char* label = MENU_POINTS[i];
        ImVec2 ts = ImGui::CalcTextSize(label);
        ImVec2 bpos(x, tl.y);
        ImVec2 bsize(ts.x + 20, TITLEBAR_H);
        ImGui::SetCursorScreenPos(bpos);
        ImGui::InvisibleButton(label, bsize);
        bool hovered = ImGui::IsItemHovered();
        if (hovered)
            dl->AddRectFilled(bpos, ImVec2(bpos.x + bsize.x, bpos.y + bsize.y),
                              u32(theme::palette().ui));
        dl->AddText(ImVec2(x + 10, tl.y + (TITLEBAR_H - ts.y) * 0.5f),
                    u32(hovered ? p.light : p.text), label);
        x += bsize.x;
    }
    ImGui::PopFont();
    ImGui::PopStyleVar(2);

    // --- right: window controls -----------------------------------------
    float bx = br.x - 46 * 3;
    ImGui::SetCursorScreenPos(ImVec2(bx, tl.y));
    if (window_button("##min", 0, win))
        glfwIconifyWindow(win);
    ImGui::SetCursorScreenPos(ImVec2(bx + 46, tl.y));
    if (window_button("##max", is_maximized(win) ? 2 : 1, win))
        toggle_maximize(win);
    ImGui::SetCursorScreenPos(ImVec2(bx + 92, tl.y));
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

} // namespace

// Default panel layout, built with DockBuilder the first time. Panel windows
// are Begin()'d by the caller under these exact names.
void build_default_layout(ImGuiID dock_id, ImVec2 size) {
    ImGui::DockBuilderRemoveNode(dock_id);
    ImGui::DockBuilderAddNode(dock_id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dock_id, size);

    ImGuiID center = dock_id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.30f, nullptr, &center);

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
                             ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoDocking;
    ImGui::Begin("##host", nullptr, flags);
    ImGui::PopStyleVar(3);

    resize_handles(window);
    titlebar(window);

    ImGuiID dock_id = ImGui::GetID("##dockspace");
    const ImVec2 dock_size(vp->Size.x, vp->Size.y - TITLEBAR_H);

    g_first_run = (ImGui::DockBuilderGetNode(dock_id) == nullptr);
    if (g_first_run)
        build_default_layout(dock_id, dock_size);

    ImGui::SetCursorScreenPos(ImVec2(vp->Pos.x, vp->Pos.y + TITLEBAR_H));
    ImGui::DockSpace(dock_id, dock_size, ImGuiDockNodeFlags_None);
    return dock_id;
}

void end() { ImGui::End(); }

bool first_run() { return g_first_run; }

} // namespace shell
