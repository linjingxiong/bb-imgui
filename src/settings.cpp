#include "settings.h"

#include "bb.h"
#include "fonts.h"
#include "icons.h"
#include "theme.h"

#include "imgui.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace settings {
namespace {

Settings g_s;
bool g_open = false;
int g_page = 0;

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

std::string exe_dir() {
    char buf[4096] = {0};
#if defined(_WIN32)
    GetModuleFileNameA(nullptr, buf, sizeof(buf));
#elif defined(__APPLE__)
    uint32_t n = sizeof(buf);
    _NSGetExecutablePath(buf, &n);
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) buf[n] = 0;
#endif
    std::string p(buf);
    auto slash = p.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : p.substr(0, slash);
}
std::string path() { return exe_dir() + "/settings.json"; }

// ── setting rows (css/dialogs.css .settings_list) ───────────────────────
constexpr float ROW_GAP = 14.0f;

void row_label(const char* name, const char* desc, float x, float w) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::PushFont(fonts::medium(), theme::size::BODY); // .setting_name 1.1em
    dl->AddText(ImVec2(x, c.y), u32(p.text), name);
    float nh = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::size::SMALL); // .setting_description 0.94em
    dl->AddText(ImVec2(x, c.y + nh + 1.0f), u32(p.subtle_text), desc);
    float dh = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    (void)w;
    ImGui::Dummy(ImVec2(1.0f, nh + dh + 3.0f));
}

// Toggle / number: control in a ~52px left column, label to its right.
void s_toggle(const char* name, const char* desc, bool* v) {
    ImGui::PushID(name);
    ImVec2 c = ImGui::GetCursorScreenPos();
    ImGui::SetCursorScreenPos(ImVec2(c.x + 2.0f, c.y + 4.0f));
    bool changed = bb::toggle("##t", v);
    ImGui::SetCursorScreenPos(c);
    row_label(name, desc, c.x + 58.0f, ImGui::GetContentRegionAvail().x - 58.0f);
    ImGui::Dummy(ImVec2(1.0f, ROW_GAP));
    if (changed) save();
    ImGui::PopID();
}

void s_number(const char* name, const char* desc, int* v, int mn, int mx) {
    ImGui::PushID(name);
    ImVec2 c = ImGui::GetCursorScreenPos();
    double d = *v;
    bb::NumOpts o;
    o.step = 1.0;
    o.min = mn;
    o.max = mx;
    o.decimals = 0;
    o.width = 52.0f;
    ImGui::SetCursorScreenPos(ImVec2(c.x, c.y + 3.0f));
    bool changed = bb::num_slider("##n", &d, o);
    if (changed) { *v = (int)(d + (d < 0 ? -0.5 : 0.5)); save(); }
    ImGui::SetCursorScreenPos(c);
    row_label(name, desc, c.x + 64.0f, ImGui::GetContentRegionAvail().x - 64.0f);
    ImGui::Dummy(ImVec2(1.0f, ROW_GAP));
    ImGui::PopID();
}

// Combo / text: label stacked above a full-width control.
bool s_combo(const char* name, const char* desc, int* cur, const char* const items[], int n) {
    ImGui::PushID(name);
    ImVec2 c = ImGui::GetCursorScreenPos();
    row_label(name, desc, c.x, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    bool changed = bb::combo("##c", cur, items, n);
    ImGui::Dummy(ImVec2(1.0f, ROW_GAP));
    if (changed) save();
    ImGui::PopID();
    return changed;
}

void s_text(const char* name, const char* desc, std::string* v) {
    ImGui::PushID(name);
    ImVec2 c = ImGui::GetCursorScreenPos();
    row_label(name, desc, c.x, ImGui::GetContentRegionAvail().x);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (bb::input_text("##x", v)) save();
    ImGui::Dummy(ImVec2(1.0f, ROW_GAP));
    ImGui::PopID();
}

void s_info(const char* name, const char* text) {
    const theme::Palette& p = theme::palette();
    ImGui::PushFont(fonts::medium(), theme::size::BODY);
    ImGui::TextColored(p.text, "%s", name);
    ImGui::PopFont();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::PushStyleColor(ImGuiCol_Text, u32(p.subtle_text));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(1.0f, ROW_GAP));
}

// ── pages ──────────────────────────────────────────────────────────────
const char* const PAGES[] = {"General", "Playback", "Video", "About"};
constexpr int N_PAGES = 4;

void page_general() {
    // Theme — driven straight into the live theme registry.
    const auto& names = theme::list();
    std::vector<const char*> items;
    for (const auto& s : names) items.push_back(s.c_str());
    int cur = 0;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == theme::current()) cur = (int)i;
    if (!items.empty() && s_combo("Theme", "Interface + plot colours", &cur, items.data(),
                                  (int)items.size())) {
        theme::set(names[cur]);
        g_s.theme = names[cur];
        save();
    }
    s_toggle("Autoplay on open", "Start playing as soon as a recording is loaded",
             &g_s.autoplay_on_open);
}

void page_playback() {
    static const char* SPEEDS[] = {"0.5x", "1x", "2x", "4x"};
    static const float SPV[] = {0.5f, 1.0f, 2.0f, 4.0f};
    int si = 1;
    for (int i = 0; i < 4; ++i)
        if (SPV[i] == g_s.default_speed) si = i;
    if (s_combo("Default speed", "Playback speed a recording opens at", &si, SPEEDS, 4)) {
        g_s.default_speed = SPV[si];
        save();
    }
    s_toggle("Loop at end", "Jump back to the start when playback reaches the end",
             &g_s.loop_at_end);
}

void page_video() {
    static const char* ROT[] = {"0\xc2\xb0", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
    int ri = (((g_s.default_rotation % 360) + 360) % 360) / 90;
    if (s_combo("Default rotation", "Orientation new video panels open at (the Ego cameras "
                                    "are mounted sideways)",
                &ri, ROT, 4)) {
        g_s.default_rotation = ri * 90;
        save();
    }
    static const char* FIT[] = {"Contain", "Cover"};
    s_combo("Default fit", "Contain letterboxes the frame; Cover crops it to fill",
            &g_s.default_fit, FIT, 2);
}

void page_about() {
    s_info("bb-imgui MCAP player",
           "A Foxglove-style MCAP player in Blockbench's visual style. Video decode + "
           "playback engine ported from EgoViewer (Qt) to plain C++/wgpu.");
    s_info("Build", "Dear ImGui (docking) + wgpu-native + FFmpeg. mcap-player-foxglove branch.");
}

// ── dialog chrome ──────────────────────────────────────────────────────
void draw_titlebar(ImVec2 tl, float w, float h) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(tl, ImVec2(tl.x + w, tl.y + h), u32(p.frame));

    ImGui::PushFont(fonts::body(), 16.0f);
    ImVec2 ch = ImGui::CalcTextSize(ICON_EXPAND_MORE);
    dl->AddText(ImVec2(tl.x + 12, tl.y + (h - ch.y) * 0.5f), u32(p.subtle_text), ICON_EXPAND_MORE);
    ImGui::PopFont();
    ImGui::PushFont(fonts::medium(), theme::size::BODY);
    ImVec2 ts = ImGui::CalcTextSize("Settings");
    dl->AddText(ImVec2(tl.x + 34, tl.y + (h - ts.y) * 0.5f), u32(p.light), "Settings");
    ImGui::PopFont();

    ImVec2 x0(tl.x + w - h, tl.y);
    ImGui::SetCursorScreenPos(x0);
    ImGui::InvisibleButton("##close", ImVec2(h, h));
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked()) { g_open = false; ImGui::CloseCurrentPopup(); }
    if (hov) dl->AddRectFilled(x0, ImVec2(x0.x + h, x0.y + h), IM_COL32(232, 63, 66, 255));
    ImVec2 cc(x0.x + h * 0.5f, x0.y + h * 0.5f);
    ImU32 xc = u32(hov ? p.light : p.subtle_text);
    dl->AddLine(ImVec2(cc.x - 5, cc.y - 5), ImVec2(cc.x + 5, cc.y + 5), xc, 1.4f);
    dl->AddLine(ImVec2(cc.x - 5, cc.y + 5), ImVec2(cc.x + 5, cc.y - 5), xc, 1.4f);
}

void draw_nav(float w, float h) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h), u32(p.back));

    ImGui::BeginChild("##nav", ImVec2(w, h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(1, 10));
    for (int i = 0; i < N_PAGES; ++i) {
        ImVec2 rp = ImGui::GetCursorScreenPos();
        ImVec2 rs(w, 28.0f);
        ImGui::PushID(i);
        ImGui::InvisibleButton("p", rs);
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) g_page = i;
        ImGui::PopID();
        bool sel = (g_page == i);
        if (sel) {
            dl->AddRectFilled(rp, ImVec2(rp.x + w, rp.y + rs.y), u32(p.ui));
            dl->AddRectFilled(rp, ImVec2(rp.x + 4, rp.y + rs.y), u32(p.accent));
        }
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float th = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(rp.x + 20, rp.y + (rs.y - th) * 0.5f),
                    u32(sel || hov ? p.light : p.text), PAGES[i]);
        ImGui::PopFont();
    }

    // Bottom actions (border-top: 2px --color-border).
    float used = ImGui::GetCursorPosY();
    float actions_h = 2 * 30.0f + 16.0f;
    ImGui::SetCursorPosY(std::max(used + 12.0f, h - actions_h));
    ImVec2 ap = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(ap.x, ap.y), ImVec2(ap.x + w, ap.y), u32(p.border), 2.0f);
    ImGui::Dummy(ImVec2(1, 8));
    auto action = [&](const char* icon, const char* label) {
        ImVec2 rp = ImGui::GetCursorScreenPos();
        ImVec2 rs(w, 28.0f);
        ImGui::PushID(label);
        ImGui::InvisibleButton("a", rs);
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        ImGui::PopID();
        ImGui::PushFont(fonts::body(), 16.0f);
        ImVec2 is = ImGui::CalcTextSize(icon);
        dl->AddText(ImVec2(rp.x + 12, rp.y + (rs.y - is.y) * 0.5f), u32(hov ? p.light : p.text),
                    icon);
        ImGui::PopFont();
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float th = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(rp.x + 38, rp.y + (rs.y - th) * 0.5f), u32(hov ? p.light : p.text),
                    label);
        ImGui::PopFont();
        return clk;
    };
    if (action(ICON_FOLDER_OPEN, "Import Settings")) {
        // Re-read the file from disk (a hand-edited settings.json).
        load();
    }
    if (action(ICON_SAVE, "Export Settings")) save();

    ImGui::EndChild();
}

} // namespace

Settings& get() { return g_s; }

void load() {
    std::ifstream f(path(), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    try {
        auto j = nlohmann::json::parse(ss.str());
        g_s.theme = j.value("theme", g_s.theme);
        g_s.autoplay_on_open = j.value("autoplay_on_open", g_s.autoplay_on_open);
        g_s.loop_at_end = j.value("loop_at_end", g_s.loop_at_end);
        g_s.default_speed = j.value("default_speed", g_s.default_speed);
        g_s.default_rotation = j.value("default_rotation", g_s.default_rotation);
        g_s.default_fit = j.value("default_fit", g_s.default_fit);
        g_s.layout = j.value("layout", g_s.layout);
    } catch (...) {
    }
    if (!g_s.theme.empty()) theme::set(g_s.theme);
}

void save() {
    nlohmann::json j;
    j["theme"] = g_s.theme;
    j["autoplay_on_open"] = g_s.autoplay_on_open;
    j["loop_at_end"] = g_s.loop_at_end;
    j["default_speed"] = g_s.default_speed;
    j["default_rotation"] = g_s.default_rotation;
    j["default_fit"] = g_s.default_fit;
    j["layout"] = g_s.layout;
    std::ofstream f(path(), std::ios::binary | std::ios::trunc);
    if (f) f << j.dump(2);
}

void open() { g_open = true; }
bool is_open() { return g_open; }

void draw() {
    if (g_open && !ImGui::IsPopupOpen("##settings"))
        ImGui::OpenPopup("##settings");
    if (!g_open) return;

    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 dsz(std::min(960.0f, vp->Size.x - 80.0f), std::min(640.0f, vp->Size.y - 100.0f));
    ImGui::SetNextWindowSize(dsz);
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    // PopupBg is the bright context-menu colour; a dialog wants the panel bg.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, u32(theme::palette().ui));
    bool stay = true;
    if (ImGui::BeginPopupModal("##settings", &stay,
                               ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar)) {
        const theme::Palette& p = theme::palette();
        ImVec2 tl = ImGui::GetCursorScreenPos();
        const float TITLE_H = 36.0f, BAR_H = 48.0f, NAV_W = 200.0f;
        float body_h = dsz.y - TITLE_H - BAR_H;

        draw_titlebar(tl, dsz.x, TITLE_H);

        ImGui::SetCursorScreenPos(ImVec2(tl.x, tl.y + TITLE_H));
        draw_nav(NAV_W, body_h);

        ImGui::SetCursorScreenPos(ImVec2(tl.x + NAV_W, tl.y + TITLE_H));
        ImGui::GetWindowDrawList()->AddLine(ImVec2(tl.x + NAV_W, tl.y + TITLE_H),
                                            ImVec2(tl.x + NAV_W, tl.y + TITLE_H + body_h),
                                            u32(p.border), 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 18));
        ImGui::BeginChild("##content", ImVec2(dsz.x - NAV_W, body_h), ImGuiChildFlags_None);
        // Inputs need to read against the dialog's p.ui background.
        ImGui::PushStyleColor(ImGuiCol_FrameBg, u32(p.deep));
        ImGui::PushStyleColor(ImGuiCol_Border,
                              u32(ImVec4(p.subtle_text.x, p.subtle_text.y, p.subtle_text.z, 0.4f)));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8, 6));

        ImGui::PushFont(fonts::medium(), 26.0f);
        ImGui::TextColored(p.text, "%s", PAGES[g_page]);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(1, 12));

        switch (g_page) {
            case 0: page_general(); break;
            case 1: page_playback(); break;
            case 2: page_video(); break;
            case 3: page_about(); break;
        }

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(2);
        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Bottom bar — Close, right-aligned (css .dialog_bar.button_bar).
        ImVec2 bp(tl.x, tl.y + TITLE_H + body_h);
        ImGui::GetWindowDrawList()->AddRectFilled(bp, ImVec2(bp.x + dsz.x, bp.y + BAR_H),
                                                  u32(p.ui));
        ImGui::GetWindowDrawList()->AddLine(bp, ImVec2(bp.x + dsz.x, bp.y), u32(p.border), 1.0f);
        float bw = 88.0f;
        ImGui::SetCursorScreenPos(ImVec2(bp.x + dsz.x - bw - 14.0f, bp.y + (BAR_H - 30.0f) * 0.5f));
        if (bb::button("Close", ImVec2(bw, 30.0f))) {
            g_open = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    if (!stay) g_open = false;
}

} // namespace settings
