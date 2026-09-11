#include "player_ui.h"

#include "bb.h"
#include "fonts.h"
#include "icons.h"
#include "pixel_inspector.h"
#include "pixel_sample.h"
#include "playback.h"
#include "settings.h"
#include "theme.h"
#include "video_texture.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // keep the windows.h min/max macros from breaking std::min({...})
#endif
#include <windows.h>
#include <commdlg.h>
#include <shobjidl.h>
#endif

namespace player_ui {
namespace {

WGPUDevice g_device = nullptr;
WGPUQueue g_queue = nullptr;
std::unique_ptr<mp::Playback> g_pb;
std::map<std::string, std::unique_ptr<mp::VideoTexture>> g_textures;
std::string g_selected_topic;
int g_rotation = 0; // degrees CW (seed until a file opens; then settings.default_rotation)
// Adaptive per-topic IMU Y-scale — grows to the largest |value| seen, never
// shrinks (EgoViewer's accMaxSeen_/gyroMaxSeen_). Reset on file open.
std::map<std::string, float> g_imu_scale;

// Per-video-panel view state + the one panel (if any) expanded to fill the
// stage. Reset on file open.
struct View {
    int rot = -1;     // -1 => seed from settings on first use
    int fit = -1;     // -1 => seed; then 0 = contain (letterbox), 1 = cover (fill)
    int stats = -1;   // -1 => seed (on); then 0 = hide, 1 = show the info overlay
};
std::map<std::string, View> g_view;
std::string g_focus_topic; // panel expanded to fill the stage (temporary)
std::string g_featured;    // spotlight-layout main video
std::string g_ep_filter;   // episode-list search text (LeRobot). Reset on open.

// A dialog pick is stashed here for the next frame's poll_open() to open
// asynchronously, so the heavy load never runs inside the UI callback.
std::mutex g_open_mx;
std::string g_pending_open_path;
bool g_await_post_open = false; // an async open() is in flight; finish setup when it lands

// The one video panel (if any) currently showing the sensor inset, and which
// tab (0 = accel, 1 = gyro, 2 = audio). Only one at a time. Reset on open.
std::string g_sensor_panel;
int g_sensor_tab = 0;
// Per scalar-channel: which traces are hidden (click the legend to toggle).
// Sized to the channel's dim count on first use. Reset on open.
std::map<std::string, std::vector<bool>> g_axis_hidden;

// Colour for trace `k` of a `dims`-trace scalar channel. First three reuse the
// axis palette (IMU xyz); beyond that, evenly spaced hues.
ImVec4 trace_color(int k, int dims) {
    static const ImVec4 axis[3] = {theme::axis::X, theme::axis::Y, theme::axis::Z};
    if (dims <= 3 && k < 3) return axis[k];
    float h = (float)k / (float)std::max(1, dims);
    float r = 0, g = 0, b = 0;
    ImGui::ColorConvertHSVtoRGB(h, 0.55f, 0.95f, r, g, b);
    return ImVec4(r, g, b, 1.0f);
}

void rotate_all() {
    g_rotation = (g_rotation + 90) % 360;
    for (auto& kv : g_view)
        kv.second.rot = ((((kv.second.rot % 360) + 360) % 360) + 90) % 360;
}

// Layout metrics. The right panel width is user-draggable.
constexpr float RAIL_W = 48.0f;
constexpr float TRANSPORT_H = 44.0f;
constexpr float TOOLBAR_H = 32.0f; // top status strip over the video stage
constexpr float PANEL_W_MIN = 260.0f;
constexpr float PANEL_W_MAX = 640.0f;
float g_panel_w = PANEL_W_MIN; // opens at the minimum width; drag to widen
bool g_panel_hidden = false; // left dock panel (rail toggle); shown by default

// Icon sizes (Blockbench: .material-icons 22px, .tool 36x30).
constexpr float RAIL_ICON_PX = 24.0f;

// Per-axis plot colours — Blockbench's viewport axis colours (css/setup.css
// --color-axis-{x,y,z}), same triplet EgoViewer's SensorPanel uses.
const ImVec4 kAxisR = theme::axis::X;
const ImVec4 kAxisG = theme::axis::Y;
const ImVec4 kAxisB = theme::axis::Z;

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }
ImVec2 snap(ImVec2 v) { return ImVec2(std::floor(v.x + 0.5f), std::floor(v.y + 0.5f)); }

// Draw an icon glyph optically centred in [box_min, box_max] at pixel `px`.
// Material Symbols render ~10% high against their text metrics, so nudge
// down; snap the result to a whole pixel to keep the edges crisp.
void icon_centered(ImDrawList* dl, const char* glyph, ImVec2 box_min, ImVec2 box_max, float px,
                   ImU32 col, bool bold = false) {
    ImGui::PushFont(fonts::body(), px);
    ImVec2 ts = ImGui::CalcTextSize(glyph);
    float cx = (box_min.x + box_max.x) * 0.5f;
    float cy = (box_min.y + box_max.y) * 0.5f;
    ImVec2 gp(std::floor(cx - ts.x * 0.5f + 0.5f), std::floor(cy - px * 0.5f + px * 0.10f + 0.5f));
    if (bold)
        for (ImVec2 o : {ImVec2(0.6f, 0), ImVec2(0, 0.6f), ImVec2(0.6f, 0.6f)})
            dl->AddText(ImVec2(gp.x + o.x, gp.y + o.y), col, glyph);
    dl->AddText(gp, col, glyph);
    ImGui::PopFont();
}

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

// Blockbench-style tooltip: bright rounded card, dark text, dim shortcut.
void tooltip(const char* text, const char* shortcut = nullptr) {
    const theme::Palette& p = theme::palette();
    ImGui::PushStyleColor(ImGuiCol_PopupBg, u32(p.bright_ui));
    ImGui::PushStyleColor(ImGuiCol_Text, u32(p.bright_ui_text));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 5));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    if (ImGui::BeginTooltip()) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextUnformatted(text);
        if (shortcut && shortcut[0]) {
            ImGui::SameLine(0, 10);
            ImGui::TextColored(mix(p.bright_ui_text, p.bright_ui, 0.5f), "%s", shortcut);
        }
        ImGui::PopFont();
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}
ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return ImVec2(a.x + b.x, a.y + b.y); }

// Draw a texture into `size` at the current cursor, rotated `rot` degrees CW.
void draw_video(ImVec2 size, ImTextureID tex, int rot) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 a(p0.x, p0.y), b(p0.x + size.x, p0.y);
    ImVec2 c(p0.x + size.x, p0.y + size.y), d(p0.x, p0.y + size.y);
    ImVec2 uv0(0, 0), uv1(1, 0), uv2(1, 1), uv3(0, 1);
    switch (((rot % 360) + 360) % 360) {
        case 90:  uv0 = {0, 1}; uv1 = {0, 0}; uv2 = {1, 0}; uv3 = {1, 1}; break;
        case 180: uv0 = {1, 1}; uv1 = {0, 1}; uv2 = {0, 0}; uv3 = {1, 0}; break;
        case 270: uv0 = {1, 0}; uv1 = {1, 1}; uv2 = {0, 1}; uv3 = {0, 0}; break;
        default: break;
    }
    ImGui::GetWindowDrawList()->AddImageQuad(tex, a, b, c, d, uv0, uv1, uv2, uv3,
                                             IM_COL32_WHITE);
    ImGui::Dummy(size);
}

const char* short_topic(const std::string& t) {
    auto pos = t.find_last_of('/');
    return pos == std::string::npos ? t.c_str() : t.c_str() + pos + 1;
}

std::string upper(const std::string& s) {
    std::string r = s;
    for (char& c : r)
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
    return r;
}

// ── Left icon rail ──────────────────────────────────────────────────────
void rail(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, u32(p.frame));
    dl->AddLine(ImVec2(pos.x + size.x - 0.5f, pos.y), ImVec2(pos.x + size.x - 0.5f, pos.y + size.y),
                u32(p.border), 1.0f);

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##rail", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const float BTN_H = 42.0f;
    auto rail_btn = [&](const char* icon, const char* tip, bool active) -> bool {
        ImVec2 bp = ImGui::GetCursorScreenPos();
        ImVec2 bs(size.x, BTN_H);
        ImGui::InvisibleButton(icon, bs);
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        if (active || hov)
            dl->AddRectFilled(bp, bp + bs, u32(p.selected));
        if (active)
            dl->AddRectFilled(bp, ImVec2(bp.x + 2.0f, bp.y + bs.y), u32(p.accent));
        icon_centered(dl, icon, bp, bp + bs, RAIL_ICON_PX, u32(hov || active ? p.light : p.text));
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (tip) tooltip(tip);
        }
        return clk;
    };
    auto rail_sep = [&] {
        ImVec2 c = ImGui::GetCursorScreenPos();
        dl->AddLine(ImVec2(c.x + 10, c.y + 4), ImVec2(c.x + size.x - 10, c.y + 4), u32(p.border));
        ImGui::Dummy(ImVec2(0, 9));
    };

    if (rail_btn(ICON_ARROW_BACK, "Close recording", false)) {
        if (g_pb) g_pb->close();
        g_textures.clear();
        g_selected_topic.clear();
        g_imu_scale.clear();
        g_axis_hidden.clear();
        g_view.clear();
        g_sensor_panel.clear();
        g_focus_topic.clear();
        g_featured.clear();
        g_ep_filter.clear();
        g_panel_hidden = false;
    }
    rail_sep();
    if (rail_btn(ICON_PHOTO_LIBRARY, "Open MCAP\xe2\x80\xa6", false)) open_dialog();
    if (rail_btn(ICON_FOLDER_OPEN, "Open LeRobot\xe2\x80\xa6", false)) open_folder_dialog();
    if (rail_btn(ICON_ROTATE, "Rotate video 90\xc2\xb0", false)) rotate_all();
    if (has_file() && rail_btn(ICON_VIEW_SIDEBAR,
                               g_panel_hidden ? "Show panel" : "Hide panel", !g_panel_hidden))
        g_panel_hidden = !g_panel_hidden;

    // Bottom: Settings, held off the rail's bottom edge.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y - BTN_H - 12.0f));
    if (rail_btn(ICON_SETTINGS, "Settings", settings::is_open())) settings::open();

    ImGui::EndChild();
}

ImVec4 fade(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// ── Shared sensor charts (right panel + video-panel inset use the same) ──
// One IMU x/y/z chart into [amin, amax]. Foxglove recorded-playback style:
// the whole recording is the X axis, the full trace is drawn once, and a
// vertical playhead sweeps across it. RAW values, adaptive symmetric Y scale,
// faint grid, Y labels, a Blockbench Transform-field style x/y/z readout row
// (values at the playhead) when inline_legend is set.
void scalar_chart(ImDrawList* dl, ImVec2 amin, ImVec2 amax, const std::string& topic,
                  bool inline_legend, ImU32 bg) {
    const theme::Palette& p = theme::palette();
    mp::Playback::ScalarSeries ser = g_pb->scalar_series(topic);
    const auto& hist = ser.samples;
    int D = std::max(1, ser.dims);
    static const char* kNum[8] = {"0", "1", "2", "3", "4", "5", "6", "7"};
    auto val = [&](const mp::ScalarSample& s, int k) -> double {
        return k < (int)s.v.size() ? (double)s.v[k] : 0.0;
    };
    auto label = [&](int k) -> const char* {
        return k < (int)ser.labels.size() && !ser.labels[k].empty() ? ser.labels[k].c_str()
                                                                    : kNum[k % 8];
    };

    const uint64_t t0 = g_pb->start_time_us();
    uint64_t t1 = g_pb->end_time_us();
    if (t1 <= t0) t1 = t0 + 1;
    const uint64_t phead = std::clamp<uint64_t>(g_pb->current_time_us(), t0, t1);

    float& scale = g_imu_scale[topic];
    if (scale <= 0.0f) scale = 1.0f;
    for (const auto& s : hist)
        for (int k = 0; k < D; ++k) scale = std::max(scale, (float)std::abs(val(s, k)));

    std::vector<double> lv(D, 0.0);
    for (const auto& s : hist) {
        if (s.t_us > phead) break;
        for (int k = 0; k < D; ++k) lv[k] = val(s, k);
    }

    const int dec = scale >= 10 ? 0 : (scale >= 1 ? 1 : 2);
    std::vector<bool>& hidden = g_axis_hidden[topic];
    if ((int)hidden.size() != D) hidden.assign(D, false);

    const float LG = 4.0f;
    float lbl_w = 0.0f;
    ImGui::PushFont(nullptr, theme::size::CAPTION);
    for (int i = 0; i <= 4; ++i) {
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.*f", dec, (1.0f - i / 2.0f) * scale);
        lbl_w = std::max(lbl_w, ImGui::CalcTextSize(lbl).x);
    }
    ImGui::PopFont();
    const float y_label_w = lbl_w + LG * 2.0f;
    const float leg_h = inline_legend ? 15.0f : 0.0f;
    ImVec2 c0(amin.x + y_label_w, amin.y + 4.0f);
    ImVec2 c1(amax.x, amax.y - 4.0f - leg_h);
    dl->AddRectFilled(c0, c1, bg);

    const ImU32 grid = u32(p.grid);
    for (int i = 0; i <= 5; ++i) {
        float gx = c0.x + (c1.x - c0.x) * i / 5.0f;
        dl->AddLine(ImVec2(gx, c0.y), ImVec2(gx, c1.y), grid, 1.0f);
    }
    ImGui::PushFont(nullptr, theme::size::CAPTION);
    for (int i = 0; i <= 4; ++i) {
        float f = 1.0f - i / 2.0f;
        float gy = c0.y + (c1.y - c0.y) * i / 4.0f;
        dl->AddLine(ImVec2(c0.x, gy), ImVec2(c1.x, gy), i == 2 ? u32(p.subtle_text) : grid, 1.0f);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.*f", dec, f * scale);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(c0.x - LG - ts.x, gy - ts.y * 0.5f), u32(p.subtle_text), lbl);
    }
    ImGui::PopFont();

    auto X = [&](uint64_t t) {
        return c0.x + (float)((double)(t - t0) / (double)(t1 - t0)) * (c1.x - c0.x);
    };
    float mid = (c0.y + c1.y) * 0.5f;
    auto Y = [&](double v) {
        return mid - (float)std::clamp(v / scale, -1.0, 1.0) * (c1.y - c0.y) * 0.48f;
    };

    bool hov = ImGui::IsMouseHoveringRect(c0, c1) &&
               !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    int hov_idx = -1;
    if (hov && !hist.empty()) {
        float mx = std::clamp(ImGui::GetIO().MousePos.x, c0.x, c1.x);
        double frac = (double)(mx - c0.x) / (double)(c1.x - c0.x);
        uint64_t ht = t0 + (uint64_t)(frac * (double)(t1 - t0));
        for (int i = 0; i < (int)hist.size(); ++i) {
            if (hist[i].t_us < t0 || hist[i].t_us > t1) continue;
            if (hist[i].t_us <= ht) hov_idx = i;
            else break;
        }
        if (hov_idx < 0) hov_idx = 0;
    }

    // Per-pixel-column decimation: keep the sample with the largest |value|
    // across all traces in each x pixel, so spikes survive the whole-file view.
    const int cols = std::max(1, (int)(c1.x - c0.x));
    std::vector<int> rep(cols, -1);
    for (int idx = 0; idx < (int)hist.size(); ++idx) {
        const auto& s = hist[idx];
        if (s.t_us < t0 || s.t_us > t1) continue;
        int cx = std::clamp((int)(X(s.t_us) - c0.x), 0, cols - 1);
        if (rep[cx] < 0) { rep[cx] = idx; continue; }
        double sm = 0, rm = 0;
        for (int k = 0; k < D; ++k) {
            sm = std::max(sm, std::abs(val(s, k)));
            rm = std::max(rm, std::abs(val(hist[rep[cx]], k)));
        }
        if (sm > rm) rep[cx] = idx;
    }

    dl->PushClipRect(c0, c1, true);
    for (int k = 0; k < D; ++k) {
        if (hidden[k]) continue;
        ImU32 col = u32(fade(trace_color(k, D), 0.9f));
        bool have = false;
        ImVec2 prev;
        for (int cx = 0; cx < cols; ++cx) {
            if (rep[cx] < 0) continue;
            const auto& s = hist[rep[cx]];
            ImVec2 pt(X(s.t_us), Y(val(s, k)));
            if (have) dl->AddLine(prev, pt, col, 1.0f);
            prev = pt;
            have = true;
        }
    }
    float hx = std::clamp(X(phead), c0.x, c1.x);
    dl->AddLine(ImVec2(hx, c0.y), ImVec2(hx, c1.y), u32(fade(p.light, 0.55f)), 1.0f);
    if (hov_idx >= 0) {
        float chx = std::clamp(X(hist[hov_idx].t_us), c0.x, c1.x);
        dl->AddLine(ImVec2(chx, c0.y), ImVec2(chx, c1.y), u32(fade(p.light, 0.9f)), 1.0f);
        for (int k = 0; k < D; ++k)
            if (!hidden[k])
                dl->AddCircleFilled(ImVec2(chx, Y(val(hist[hov_idx], k))), 2.5f,
                                    u32(trace_color(k, D)));
    }
    dl->PopClipRect();

    // Hover tooltip (foreground, unclipped): time + every trace value.
    if (hov_idx >= 0) {
        const auto& s = hist[hov_idx];
        uint64_t rel = s.t_us > t0 ? s.t_us - t0 : 0;
        char tb[24];
        std::snprintf(tb, sizeof(tb), "%llu:%02llu.%03llu",
                      (unsigned long long)(rel / 60000000ull),
                      (unsigned long long)(rel / 1000000ull % 60ull),
                      (unsigned long long)(rel / 1000ull % 1000ull));
        ImGui::PushFont(nullptr, theme::size::CAPTION);
        float lh = ImGui::GetTextLineHeight();
        std::vector<std::string> rb(D);
        float tw = ImGui::CalcTextSize(tb).x;
        char buf[48];
        for (int k = 0; k < D; ++k) {
            std::snprintf(buf, sizeof(buf), "%s % .*f", label(k), scale >= 10 ? 2 : 3, val(s, k));
            rb[k] = buf;
            tw = std::max(tw, 14.0f + ImGui::CalcTextSize(rb[k].c_str()).x);
        }
        float bw = tw + 16.0f, bh = lh * (D + 1) + 12.0f;
        ImVec2 m = ImGui::GetIO().MousePos;
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 bp(m.x + 16.0f, m.y - bh - 10.0f);
        bp.x = std::min(bp.x, vp->Pos.x + vp->Size.x - bw - 4.0f);
        bp.x = std::max(bp.x, vp->Pos.x + 4.0f);
        if (bp.y < vp->Pos.y + 4.0f) bp.y = m.y + 18.0f;
        ImDrawList* fg = ImGui::GetForegroundDrawList();
        fg->AddRectFilled(bp, ImVec2(bp.x + bw, bp.y + bh), u32(fade(p.deep, 0.97f)), 4.0f);
        fg->AddRect(bp, ImVec2(bp.x + bw, bp.y + bh), u32(fade(p.light, 0.20f)), 4.0f, 0, 1.0f);
        fg->AddText(ImVec2(bp.x + 8.0f, bp.y + 5.0f), u32(p.subtle_text), tb);
        for (int k = 0; k < D; ++k) {
            float ry = bp.y + 5.0f + lh * (k + 1);
            fg->AddRectFilled(ImVec2(bp.x + 8.0f, ry + 3.0f), ImVec2(bp.x + 14.0f, ry + 9.0f),
                              u32(trace_color(k, D)));
            fg->AddText(ImVec2(bp.x + 18.0f, ry), u32(p.text), rb[k].c_str());
        }
        ImGui::PopFont();
    }

    // Legend / readout row below the plot: a swatch + label + value per trace,
    // click to toggle that trace.
    if (inline_legend) {
        ImGui::PushFont(nullptr, theme::size::CAPTION);
        const ImVec2 cur_save = ImGui::GetCursorScreenPos();
        ImGui::PushID(topic.c_str());
        float rowy = c1.y + 3.0f;
        float cw = (amax.x - c0.x) / (float)D;
        for (int k = 0; k < D; ++k) {
            float sx = c0.x + cw * k + 2.0f;
            char t[48];
            std::snprintf(t, sizeof(t), "%s % .*f", label(k), scale >= 10 ? 2 : 3, lv[k]);
            float iw = 10.0f + ImGui::CalcTextSize(t).x;
            ImGui::SetCursorScreenPos(ImVec2(sx, rowy - 1.0f));
            ImGui::PushID(k);
            if (ImGui::InvisibleButton("ax", ImVec2(std::min(iw, cw), 14.0f)))
                hidden[k] = !hidden[k];
            bool ih = ImGui::IsItemHovered();
            ImGui::PopID();
            if (ih) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            ImVec2 s0(sx, rowy + 3.0f), s1(sx + 6.0f, rowy + 9.0f);
            if (hidden[k]) dl->AddRect(s0, s1, u32(trace_color(k, D)), 0.0f, 0, 1.0f);
            else dl->AddRectFilled(s0, s1, u32(trace_color(k, D)));
            dl->AddText(ImVec2(sx + 10.0f, rowy),
                        u32(hidden[k] ? p.subtle_text : (ih ? p.light : p.text)), t);
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(cur_save);
        ImGui::PopFont();
    }
}

// One audio peak-envelope into [amin, amax] — same whole-file X axis + playhead.
void audio_chart(ImDrawList* dl, ImVec2 amin, ImVec2 amax, ImU32 bg) {
    const theme::Palette& p = theme::palette();
    auto hist = g_pb->audio_history();
    float w = amax.x - amin.x, h = amax.y - amin.y;
    dl->AddRectFilled(amin, amax, bg, theme::RADIUS);

    const uint64_t t0 = g_pb->start_time_us();
    uint64_t t1 = g_pb->end_time_us();
    if (t1 <= t0) t1 = t0 + 1;
    const uint64_t phead = std::clamp<uint64_t>(g_pb->current_time_us(), t0, t1);

    const float cy = amin.y + h * 0.5f;
    const float amp_h = h * 0.5f - 10.0f;

    const int cols = std::max(1, (int)(w / 3.0f));
    std::vector<float> peak(cols, 0.0f);
    for (const auto& a : hist) {
        if (a.t_us < t0 || a.t_us > t1) continue;
        int col = std::clamp((int)((double)(a.t_us - t0) / (double)(t1 - t0) * cols), 0, cols - 1);
        peak[col] = std::max(peak[col], std::min(a.amp, 1.0f));
    }
    ImU32 wav = u32(mix(p.deep, p.subtle_text, 0.7f));
    for (int i = 0; i < cols; ++i) {
        if (peak[i] <= 0.0f) continue;
        float x = amin.x + (i + 0.5f) * (w / cols);
        float bh = std::max(0.5f, peak[i] * amp_h);
        dl->AddLine(ImVec2(x, cy - bh), ImVec2(x, cy + bh), wav, 1.0f);
    }
    dl->AddLine(ImVec2(amin.x, cy), ImVec2(amax.x, cy), u32(p.border), 1.0f);
    float hx = amin.x + (float)((double)(phead - t0) / (double)(t1 - t0)) * w;
    hx = std::clamp(hx, amin.x, amax.x);
    dl->AddLine(ImVec2(hx, amin.y), ImVec2(hx, amax.y), u32(fade(p.light, 0.55f)), 1.0f);
}

// The recording's sensor tabs for the inset — one per scalar channel plus an
// audio tab, in a stable order. `topic` is empty for the audio tab.
struct SensorTab {
    std::string label;
    std::string topic;
    bool audio = false;
};
struct SensorTabs {
    std::vector<SensorTab> tabs;
    bool any() const { return !tabs.empty(); }
};
SensorTabs sensor_tabs() {
    SensorTabs st;
    if (!has_file()) return st;
    for (const auto& t : g_pb->scalar_topics()) {
        std::string name = g_pb->scalar_series(t).name;
        if (!name.empty()) name[0] = (char)std::toupper((unsigned char)name[0]);
        st.tabs.push_back({name.empty() ? t : name, t, false});
    }
    if (g_pb->has_audio()) st.tabs.push_back({"Audio", "", true});
    return st;
}


// ── Video panel ────────────────────────────────────────────────────────
// A Blockbench-style panel: square, flush-tiled, a `panel_handle`-like
// header (uppercase muted title + controls that only surface on hover)
// over the video frame. The settings popup carries rotation + fit mode.
void video_panel(const std::string& topic, ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float HEAD = 32.0f; // Blockbench #center h3.panel_handle
    pos.x = std::floor(pos.x);
    pos.y = std::floor(pos.y);
    size.x = std::floor(size.x);
    size.y = std::floor(size.y);

    dl->AddRectFilled(pos, pos + size, u32(p.ui));
    dl->AddRect(pos, pos + size, u32(p.border), 0.0f, 0, 1.0f); // card floats on the stage
    dl->AddLine(ImVec2(pos.x, pos.y + HEAD), ImVec2(pos.x + size.x, pos.y + HEAD), u32(p.border),
                1.0f);

    View& v = g_view[topic];
    if (v.rot < 0) v.rot = g_rotation;
    if (v.fit < 0) v.fit = settings::get().default_fit;
    if (v.stats < 0) v.stats = 1;

    bool focused = (g_focus_topic == topic);

    // Title — uppercase, muted (Blockbench panel_handle > label, 1.1em).
    // Clipped so a long topic name never runs under the header buttons (there
    // are 2, 24px each, drawn from the right edge — see below); narrow panels
    // (e.g. the spotlight strip's thumbnails) rely on this to stay tidy.
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    dl->PushClipRect(pos, ImVec2(pos.x + std::max(0.0f, size.x - 56.0f), pos.y + HEAD), true);
    dl->AddText(snap(ImVec2(pos.x + 10, pos.y + (HEAD - ImGui::GetTextLineHeight()) * 0.5f + 1.0f)),
                u32(p.subtle_text), upper(topic).c_str());
    dl->PopClipRect();
    ImGui::PopFont();

    // Controls — always shown; Blockbench .panel_control brightens on hover
    // (opacity only, no background) and surfaces a tooltip.
    ImGui::PushID((topic + "vp").c_str());
    float rx = pos.x + size.x - 4.0f;
    // Per-icon size so the glyphs read the same visual weight: the diagonal
    // arrows fill their em box, the 3-dot menu is thin, so give the menu a
    // couple more px.
    auto hdr_btn = [&](const char* tag, const char* icon, const char* tip, bool active,
                       float px) -> bool {
        ImVec2 bs(24.0f, 24.0f);
        ImVec2 bp = snap(ImVec2(rx - bs.x, pos.y + (HEAD - bs.y) * 0.5f));
        ImGui::PushID(tag);
        ImGui::SetCursorScreenPos(bp);
        ImGui::InvisibleButton("b", bs);
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        ImGui::PopID();
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (tip) tooltip(tip);
        }
        ImVec4 c = active ? p.accent
                          : hov ? p.light : ImVec4(p.text.x, p.text.y, p.text.z, 0.8f);
        icon_centered(dl, icon, bp, bp + bs, px, u32(c), true);
        rx = bp.x;
        return clk;
    };

    if (hdr_btn("more", ICON_MORE_VERT, "Panel menu", ImGui::IsPopupOpen("vset"), 20.0f))
        ImGui::OpenPopup("vset");
    if (hdr_btn("exp", focused ? ICON_CLOSE_FULLSCREEN : ICON_OPEN_IN_FULL,
                focused ? "Exit fullscreen" : "Fullscreen", focused, 17.0f))
        g_focus_topic = focused ? std::string() : topic;

    ImGui::SetNextWindowPos(ImVec2(pos.x + size.x - 6.0f, pos.y + HEAD + 4.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    // Blockbench panel dialog: the plain dark UI slab, no border.
    ImGui::PushStyleColor(ImGuiCol_PopupBg, u32(p.ui));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    if (ImGui::BeginPopup("vset")) {
        ImDrawList* pdl = ImGui::GetWindowDrawList();

        // No title, no close button — just the toggle rows. Click outside to
        // dismiss. Blockbench-style row: square checkbox (white outline, white
        // check, no fill) on the left, body-size label on the right.
        ImGui::PushFont(nullptr, theme::size::BODY);
        const float PW = 150.0f;
        auto check_row = [&](const char* label, bool* val) {
            const float BOX = 18.0f;
            ImVec2 rp = ImGui::GetCursorScreenPos();
            float lh = ImGui::GetTextLineHeight();
            float rowh = std::max(BOX, lh) + 6.0f;
            ImGui::InvisibleButton(label, ImVec2(PW, rowh));
            bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            bool clk = ImGui::IsItemClicked();
            if (clk) *val = !*val;
            // checkbox flush left, label flush right, gap between
            ImVec2 b0(std::floor(rp.x), std::floor(rp.y + (rowh - BOX) * 0.5f));
            ImVec2 b1(b0.x + BOX, b0.y + BOX);
            if (*val) {
                pdl->AddRectFilled(b0, b1, u32(p.light), 2.0f);
                icon_centered(pdl, ICON_CHECK, b0, b1, 16.0f, u32(p.ui));
            } else {
                pdl->AddRect(b0, b1, u32(p.light), 2.0f, 0, 1.8f);
            }
            float tw = ImGui::CalcTextSize(label).x;
            pdl->AddText(ImVec2(std::floor(rp.x + PW - tw), std::floor(rp.y + (rowh - lh) * 0.5f)),
                         u32(p.light), label);
            return clk;
        };

        bool show = v.stats != 0;
        if (check_row("Info overlay", &show)) v.stats = show ? 1 : 0;
        ImGui::PopFont();

        ImGui::EndPopup();
    }
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(1);
    ImGui::PopID();

    // Content: the frame on a black bed (letterbox bars + the pre-decode state
    // read as black, like a video player).
    ImVec2 c0(pos.x + 2.0f, pos.y + HEAD + 2.0f);
    ImVec2 csz(size.x - 4.0f, size.y - HEAD - 4.0f);
    if (csz.x < 4.0f || csz.y < 4.0f) return;
    dl->AddRectFilled(c0, ImVec2(c0.x + csz.x, c0.y + csz.y), u32(p.deep)); // #101316
    dl->PushClipRect(c0, ImVec2(c0.x + csz.x, c0.y + csz.y), true);

    auto& tex = g_textures[topic];
    if (!tex) tex = std::make_unique<mp::VideoTexture>(g_device, g_queue);
    mp::VideoFramePtr frame = g_pb->latest_frame(topic);
    if (frame) tex->update(frame);

    if (tex->valid() && tex->width() > 0 && tex->height() > 0) {
        bool rot90 = ((((v.rot % 360) + 360) % 360) % 180) != 0;
        float tw = rot90 ? (float)tex->height() : (float)tex->width();
        float th = rot90 ? (float)tex->width() : (float)tex->height();
        float sc = v.fit == 0 ? std::min(csz.x / tw, csz.y / th)
                              : std::max(csz.x / tw, csz.y / th);
        float dw = std::floor(tw * sc), dh = std::floor(th * sc);
        ImVec2 ip = snap(ImVec2(c0.x + (csz.x - dw) * 0.5f, c0.y + (csz.y - dh) * 0.5f));
        ImGui::SetCursorScreenPos(ip);
        draw_video(ImVec2(dw, dh), tex->id(), v.rot);
        dl->PopClipRect();

        // Pixel magnifier / colour picker over the displayed frame.
        if (frame) {
            mp::VideoFramePtr f = frame;
            px::PixelInspector((topic + "##pxi").c_str(), ip, ImVec2(ip.x + dw, ip.y + dh),
                               f->width, f->height, v.rot,
                               [f](int x, int y, unsigned char* rgb) {
                                   return mp::sample_rgb(*f, x, y, rgb);
                               });
        }
    } else {
        dl->PopClipRect();
    }

    // ── Info overlay ──────────────────────────────────────────────────────
    // A single identity chip (top-left, same look as the IMU chip) plus a
    // small live-time readout (bottom-right). Toggled by the ⋮ menu.
    if (v.stats && csz.x > 150.0f && csz.y > 96.0f) {
        mp::Playback::VideoStats vs = g_pb->video_stats(topic);
        if (vs.valid) {
            const char* codec = vs.codec.empty() ? "?" : vs.codec.c_str();
            char id[64];
            if (vs.width > 0)
                std::snprintf(id, sizeof(id), "%d\xc3\x97%d \xc2\xb7 %.0f fps \xc2\xb7 %s", vs.width,
                              vs.height, vs.stream_fps, codec);
            else
                std::snprintf(id, sizeof(id), "%.0f fps \xc2\xb7 %s", vs.stream_fps, codec);

            ImGui::PushFont(nullptr, theme::size::SMALL);
            ImVec2 ts = ImGui::CalcTextSize(id);
            ImVec2 q0(std::floor(c0.x + 8.0f), std::floor(c0.y + 8.0f));
            ImVec2 q1(std::floor(q0.x + ts.x + 16.0f), std::floor(q0.y + ts.y + 8.0f));
            dl->AddRectFilled(q0, q1, u32(fade(p.deep, 0.72f)), 4.0f);
            dl->AddRect(q0, q1, u32(fade(p.light, 0.15f)), 4.0f, 0, 1.0f);
            dl->AddText(ImVec2(q0.x + 8.0f, std::floor(q0.y + 4.0f)), u32(p.text), id);
            ImGui::PopFont();
        }
    }

    // ── Sensor inset (bottom-left) ─────────────────────────────────────
    SensorTabs st = sensor_tabs();
    if (st.any() && csz.x > 260.0f && csz.y > 220.0f) {
        ImGui::PushID((topic + "sns").c_str());
        ImU32 chip_bg = u32(fade(p.deep, 0.72f));
        if (g_sensor_tab >= (int)st.tabs.size()) g_sensor_tab = 0;
        const char* chip_label = st.tabs.size() > 1 ? "PLOTS" : st.tabs[0].label.c_str();

        if (g_sensor_panel != topic) {
            // Not this panel's turn — a small chip to open the plots here.
            ImGui::PushFont(nullptr, theme::size::CAPTION);
            float tw2 = ImGui::CalcTextSize(chip_label).x;
            ImVec2 q0(std::floor(c0.x + 8.0f), std::floor(c0.y + csz.y - 8.0f - 22.0f));
            ImVec2 q1(std::floor(q0.x + 22.0f + tw2 + 8.0f), q0.y + 22.0f);
            ImGui::SetCursorScreenPos(q0);
            ImGui::InvisibleButton("chip", ImVec2(q1.x - q0.x, 22.0f));
            bool hov = ImGui::IsItemHovered();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked()) { g_sensor_panel = topic; g_sensor_tab = 0; }
            dl->AddRectFilled(q0, q1, chip_bg, 4.0f);
            dl->AddRect(q0, q1, u32(fade(p.light, 0.15f)), 4.0f, 0, 1.0f);
            icon_centered(dl, ICON_TIMELINE, ImVec2(q0.x, q0.y), ImVec2(q0.x + 22.0f, q1.y), 14.0f,
                          u32(hov ? p.light : p.subtle_text));
            dl->AddText(ImVec2(q0.x + 22.0f,
                               std::floor(q0.y + (22.0f - ImGui::GetTextLineHeight()) * 0.5f)),
                        u32(hov ? p.light : p.text), chip_label);
            ImGui::PopFont();
        } else {
            float iw = std::floor(std::clamp(csz.x * 0.42f, 220.0f, 310.0f));
            float ih = std::floor(std::clamp(csz.y * 0.22f, 124.0f, 150.0f));
            ImVec2 q0(std::floor(c0.x + 8.0f), std::floor(c0.y + csz.y - 8.0f - ih));
            ImVec2 q1(q0.x + iw, q0.y + ih);
            dl->AddRectFilled(q0, q1, chip_bg, 5.0f);
            dl->AddRect(q0, q1, u32(fade(p.light, 0.15f)), 5.0f, 0, 1.0f);

            // ── tab bar: Acc / Gyro / Audio + collapse chevron ──────────
            // Blockbench style: the selected tab shares the content's
            // background and joins it seamlessly; the bar itself is a
            // distinct darker-grey strip.
            const float TB = 26.0f;
            const ImU32 cbg = u32(fade(p.frame, 0.9f)); // chart / selected-tab bg
            dl->AddRectFilled(q0, ImVec2(q1.x, q0.y + TB), u32(fade(p.ui, 0.85f)), 5.0f,
                              ImDrawFlags_RoundCornersTop);
            ImGui::PushFont(nullptr, theme::size::SMALL);
            float tx = q0.x;
            for (int i = 0; i < (int)st.tabs.size(); ++i) {
                const char* nm = st.tabs[i].label.c_str();
                ImVec2 ts = ImGui::CalcTextSize(nm);
                float tbw = 12.0f + ts.x + 12.0f;
                ImVec2 t0(std::floor(tx), std::floor(q0.y));
                ImVec2 t1(std::floor(tx + tbw), std::floor(q0.y + TB));
                ImGui::SetCursorScreenPos(t0);
                ImGui::PushID(i);
                ImGui::InvisibleButton("t", ImVec2(tbw, TB));
                bool th = ImGui::IsItemHovered();
                if (th) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                if (ImGui::IsItemClicked()) g_sensor_tab = i;
                ImGui::PopID();
                bool sel = (g_sensor_tab == i);
                if (sel)
                    dl->AddRectFilled(t0, t1, cbg, 5.0f,
                                      i == 0 ? ImDrawFlags_RoundCornersTopLeft
                                             : ImDrawFlags_RoundCornersNone);
                ImU32 fg = u32(sel ? p.text : (th ? p.light : p.subtle_text));
                dl->AddText(ImVec2(std::floor(t0.x + (tbw - ts.x) * 0.5f),
                                   std::floor(t0.y + (TB - ts.y) * 0.5f)),
                            fg, nm);
                tx += tbw + 2.0f;
            }
            ImGui::PopFont();
            // collapse chevron
            ImVec2 v0(q1.x - TB, q0.y), v1(q1.x, q0.y + TB);
            ImGui::SetCursorScreenPos(v0);
            ImGui::InvisibleButton("collapse", ImVec2(TB, TB));
            bool vh = ImGui::IsItemHovered();
            if (vh) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsItemClicked()) g_sensor_panel.clear();
            icon_centered(dl, ICON_CARET_DOWN, v0, v1, 16.0f, u32(vh ? p.light : p.subtle_text));

            // ── chart — over a translucent bg (the active tab names it) ────
            ImVec2 cmin(q0.x + 2.0f, std::floor(q0.y + TB));
            ImVec2 cmax(q1.x - 2.0f, q1.y - 3.0f);
            // Bridge the seam so the selected tab flows straight into the chart
            // (scalar_chart insets its own bg fill by 4px at the top).
            dl->AddRectFilled(ImVec2(cmin.x, q0.y + TB - 1.0f), ImVec2(cmax.x, cmin.y + 6.0f), cbg);
            const SensorTab& active = st.tabs[std::clamp(g_sensor_tab, 0, (int)st.tabs.size() - 1)];
            if (active.audio) audio_chart(dl, cmin, cmax, cbg);
            else scalar_chart(dl, cmin, cmax, active.topic, true, cbg);
        }
        ImGui::PopID();
    }
}

// Rotated display aspect (w/h) of a video topic. Falls back to the coded size
// learned at open() so the layout is stable before the first frame decodes;
// 0 only if the resolution is genuinely unknown.
float video_ar(const std::string& topic) {
    int w = 0, h = 0;
    auto it = g_textures.find(topic);
    if (it != g_textures.end() && it->second && it->second->valid()) {
        w = it->second->width();
        h = it->second->height();
    }
    if ((w <= 0 || h <= 0) && has_file()) {
        auto vs = g_pb->video_stats(topic);
        w = vs.width;
        h = vs.height;
    }
    if (w <= 0 || h <= 0) return 0.0f;
    int rot = g_view.count(topic) ? g_view[topic].rot : g_rotation;
    bool r90 = ((((rot % 360) + 360) % 360) % 180) != 0;
    return (float)(r90 ? h : w) / (float)(r90 ? w : h);
}

// Fit a HEAD-topped panel of video aspect `ar` inside a `cw`x`ch` cell.
ImVec2 fit_panel(float cw, float ch, float ar, float head) {
    if (ar <= 0.0f) return ImVec2(cw, ch);
    float pw = cw, ph = (pw - 4.0f) / ar + head + 4.0f;
    if (ph > ch) { ph = ch; pw = (ph - head - 4.0f) * ar + 4.0f; }
    return ImVec2(std::floor(pw), std::floor(ph));
}

// ── Preview top toolbar ──────────────────────────────────────────────────
// A thin strip pinned to the top of the video stage, mirroring the bottom
// transport bar's look (same fill, a hairline separating it from the video
// grid below). Otherwise empty — a scaffold to hang per-state info on later.
void preview_toolbar(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    pos = snap(pos);
    size = ImVec2(std::floor(size.x), std::floor(size.y));

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##toolbar", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, u32(p.ui));
    dl->AddLine(ImVec2(pos.x, pos.y + size.y), ImVec2(pos.x + size.x, pos.y + size.y),
               u32(p.border), 1.0f);

    // ── Right: layout indicator / toggle. Always shows the current layout's
    // icon; only turns into a real multi-option switch once there's a crowd
    // of cameras (spotlight is meaningless otherwise, so it isn't offered).
    if (has_file()) {
        const bool multi = g_pb->video_topics().size() > 3;
        const float box = 26.0f, cy = std::floor(pos.y + size.y * 0.5f);
        float bx = pos.x + size.x - 10.0f - box;
        int& lay = settings::get().layout;
        auto lay_btn = [&](const char* icon, const char* tip, bool active, bool clickable) {
            ImVec2 bs(box, box), bp = snap(ImVec2(bx, cy - box * 0.5f));
            bool hov = false, clk = false;
            if (clickable) {
                ImGui::PushID(icon);
                ImGui::SetCursorScreenPos(bp);
                ImGui::InvisibleButton("b", bs);
                hov = ImGui::IsItemHovered();
                clk = ImGui::IsItemClicked();
                ImGui::PopID();
                if (hov) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); tooltip(tip); }
            }
            if (active) dl->AddRectFilled(bp, bp + bs, u32(p.selected), 4.0f);
            icon_centered(dl, icon, bp, bp + bs, 16.0f,
                         u32(active ? p.light : (hov ? p.light : p.subtle_text)));
            bx -= box + 4.0f;
            return clk;
        };
        if (multi) {
            if (lay_btn(ICON_VIEW_SIDEBAR, "Layout: spotlight", lay == 1, true)) {
                lay = 1;
                settings::save();
            }
            if (lay_btn(ICON_GRID_VIEW, "Layout: grid", lay == 0, true)) {
                lay = 0;
                settings::save();
            }
        } else {
            // Nothing to switch to — just show the (always-grid) icon.
            lay_btn(ICON_GRID_VIEW, "Layout: grid", true, false);
        }
    }

    ImGui::EndChild();
}

// ── Centre video stage ─────────────────────────────────────────────────
void display(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##display", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // Blockbench-style checkerboard stage (its transparency-canvas look).
    {
        const ImU32 ck0 = u32(p.checkerboard);
        const ImU32 ck1 = u32(mix(p.checkerboard, p.light, 0.06f));
        const float cell = 12.0f;
        dl->AddRectFilled(pos, pos + size, ck0);
        dl->PushClipRect(pos, pos + size, true);
        int cols = (int)(size.x / cell) + 1, rows = (int)(size.y / cell) + 1;
        for (int r = 0; r < rows; ++r)
            for (int c = (r & 1); c < cols; c += 2) {
                ImVec2 cp(pos.x + c * cell, pos.y + r * cell);
                dl->AddRectFilled(cp, ImVec2(cp.x + cell, cp.y + cell), ck1);
            }
        dl->PopClipRect();
    }

    const bool ready = has_file() && !g_pb->video_topics().empty();
    if (!ready) {
        const bool loading = g_pb && g_pb->opening();
        const char* line1 = loading         ? "Opening\xe2\x80\xa6"
                            : has_file()     ? "This recording has no video channels."
                                             : "No recording open";
        const char* line2 =
            loading || has_file()
                ? ""
                : "Open an MCAP file or a LeRobot dataset from the rail on the left.";
        ImGui::PushFont(fonts::medium(), theme::size::HEADING);
        ImVec2 t1 = ImGui::CalcTextSize(line1);
        dl->AddText(ImVec2(pos.x + (size.x - t1.x) * 0.5f, pos.y + size.y * 0.5f - 24),
                    u32(p.subtle_text), line1);
        ImGui::PopFont();
        if (line2[0]) {
            ImGui::PushFont(nullptr, theme::size::SMALL);
            ImVec2 t2 = ImGui::CalcTextSize(line2);
            dl->AddText(ImVec2(pos.x + (size.x - t2.x) * 0.5f, pos.y + size.y * 0.5f + 4),
                        u32(p.subtle_text), line2);
            ImGui::PopFont();
        }
        ImGui::EndChild();
        return;
    }

    const auto& vts = g_pb->video_topics();

    const float GAP = 6.0f;
    const float HEAD = 32.0f; // must match video_panel's header
    // An equal margin around the video area, inset from the stage edges.
    const float PAD = 14.0f;
    const ImVec2 ipos(std::floor(pos.x + PAD), std::floor(pos.y + PAD));
    const ImVec2 isize(std::floor(size.x - 2.0f * PAD), std::floor(size.y - 2.0f * PAD));

    // Centre a single panel, sized to the video aspect, in the video area.
    auto one = [&](const std::string& topic) {
        ImVec2 ps = fit_panel(isize.x, isize.y, video_ar(topic), HEAD);
        ImVec2 pp(ipos.x + (isize.x - ps.x) * 0.5f, ipos.y + (isize.y - ps.y) * 0.5f);
        video_panel(topic, snap(pp), ps);
    };

    bool focus_valid =
        !g_focus_topic.empty() &&
        std::find(vts.begin(), vts.end(), g_focus_topic) != vts.end();
    if (focus_valid) {
        one(g_focus_topic);
        ImGui::EndChild();
        return;
    }

    const int n = (int)vts.size();

    if (n == 1) {
        one(vts[0]);
        ImGui::EndChild();
        return;
    }

    if (settings::get().layout == 1 && n > 3) {
        // ── Spotlight: one feature video + a scrolling strip of the rest ──
        if (g_featured.empty() ||
            std::find(vts.begin(), vts.end(), g_featured) == vts.end())
            g_featured = vts[0];

        // Fit the featured card against a reserved-width budget (its own
        // aspect ratio decides the rest — it's usually height-bound, so it
        // ends up narrower than the budget, with letterbox slack left over).
        // Left-anchored (not centred) so that slack all lands on its right —
        // between it and the strip — rather than splitting to its left too;
        // that keeps the card flush against the left panel at a constant PAD,
        // matching the stage's own edge margin. The strip then starts a fixed
        // PAD after the card's *actual* right edge (same margin as the other
        // two sides, not the smaller inter-panel GAP), and takes whatever
        // width is left over (floored at a minimum so a wide-aspect card that
        // eats the whole budget doesn't crowd it out) — so all three gaps
        // (left panel↔card, card↔strip, strip↔window edge) read as one PAD.
        const float MIN_STRIP_W = 180.0f;
        float feat_w = std::floor(isize.x - MIN_STRIP_W - PAD);
        ImVec2 fs = fit_panel(feat_w, isize.y, video_ar(g_featured), HEAD);
        ImVec2 fp(ipos.x, ipos.y + (isize.y - fs.y) * 0.5f);
        video_panel(g_featured, snap(fp), fs);

        // Transparent — shares the checkerboard stage bg painted above rather
        // than a flat slab that reads as a mismatched patch when the strip's
        // thumbnails don't fill the full column height. Aligned to the
        // featured panel's actual top/bottom edge (fp/fs), not the stage's.
        ImVec2 sp(std::floor(fp.x + fs.x + PAD), fp.y);
        const float STRIP_W = std::max(MIN_STRIP_W, ipos.x + isize.x - sp.x);
        ImGui::SetCursorScreenPos(sp);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, 0);
        ImGui::BeginChild("##strip", ImVec2(STRIP_W, fs.y), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        float tw = ImGui::GetContentRegionAvail().x;
        float thumb_h = std::floor(tw * 0.66f + 27.0f);

        std::vector<std::string> thumbs;
        for (const auto& t : vts)
            if (t != g_featured) thumbs.push_back(t);
        const int m = (int)thumbs.size();
        // When the thumbnails don't fill the strip, spread the leftover height
        // evenly between them (first flush to the top, last flush to the
        // bottom) instead of leaving them clumped at the top with a dead gap
        // below. Too many to fit: fall back to a fixed gap and let it scroll
        // (wheel-only — no scrollbar; edge triangles below hint at the rest).
        const bool fits = m > 1 && (m * thumb_h + (m - 1) * GAP) <= fs.y;
        const float gap = fits ? (fs.y - m * thumb_h) / (m - 1) : GAP;
        for (int i = 0; i < m; ++i) {
            const std::string& t = thumbs[i];
            ImVec2 tp = ImGui::GetCursorScreenPos();
            ImGui::PushID(t.c_str());
            ImGui::InvisibleButton("promote", ImVec2(tw, thumb_h));
            bool hov = ImGui::IsItemHovered();
            if (hov) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                tooltip("Show in main view");
            }
            if (ImGui::IsItemClicked()) g_featured = t;
            ImGui::PopID();
            video_panel(t, tp, ImVec2(tw, thumb_h));
            ImGui::SetCursorScreenPos(ImVec2(tp.x, tp.y + thumb_h));
            // Trailing gap after the last card only while scrolling (keeps the
            // scroll extent right) — when everything fits, the last card sits
            // flush at the bottom instead.
            if (i + 1 < m || !fits) ImGui::Dummy(ImVec2(tw, gap));
        }
        const float scroll_y = ImGui::GetScrollY(), scroll_max = ImGui::GetScrollMaxY();
        ImGui::EndChild();
        ImGui::PopStyleColor();

        // Edge triangles instead of a scrollbar: hint there's more content
        // above/below the current scroll position.
        if (!fits) {
            const ImU32 tri_col = u32(p.subtle_text);
            const float tw2 = 8.0f, th2 = 5.0f, tcx = sp.x + STRIP_W * 0.5f;
            if (scroll_y > 1.0f) {
                float ty = sp.y + 4.0f;
                dl->AddTriangleFilled(ImVec2(tcx - tw2 * 0.5f, ty + th2), ImVec2(tcx + tw2 * 0.5f, ty + th2),
                                      ImVec2(tcx, ty), tri_col);
            }
            if (scroll_y < scroll_max - 1.0f) {
                float ty = sp.y + fs.y - 4.0f;
                dl->AddTriangleFilled(ImVec2(tcx - tw2 * 0.5f, ty - th2), ImVec2(tcx + tw2 * 0.5f, ty - th2),
                                      ImVec2(tcx, ty), tri_col);
            }
        }
        ImGui::EndChild();
        return;
    }

    // ── Grid: 2 columns, a lone last cell centred; panels sized to the video
    //    aspect and the whole block centred in the stage (no letterbox band). ──
    // The gap between panels matches the margin around the video area (PAD).
    int rows = (n + 1) / 2;
    float cw = std::floor((isize.x - PAD) / 2.0f);
    float ch = std::floor((isize.y - PAD * (rows - 1)) / rows);
    ImVec2 ps = fit_panel(cw, ch, video_ar(vts[0]), HEAD);
    float block_h = rows * ps.y + PAD * (rows - 1);
    float y0 = ipos.y + std::floor((isize.y - block_h) * 0.5f);
    float x0 = ipos.x + std::floor((isize.x - (2.0f * ps.x + PAD)) * 0.5f);
    for (int i = 0; i < n; ++i) {
        int gy = i / 2, gx = i % 2;
        int in_row = (gy == rows - 1) ? (n - gy * 2) : 2;
        float px = (in_row == 1) ? (ipos.x + std::floor((isize.x - ps.x) * 0.5f))
                                 : (x0 + gx * (ps.x + PAD));
        video_panel(vts[i], snap(ImVec2(px, y0 + gy * (ps.y + PAD))), ps);
    }

    ImGui::EndChild();
}

// ── Bottom transport bar (Foxglove layout) ─────────────────────────────
// A thin full-width scrubber on top; a controls row below: an info toggle
// + absolute wall-clock stamp on the left, skip-start / play / skip-end
// centred, loop + speed on the right. Outline icons, no filled accent.
void transport(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    pos = snap(pos);
    size = ImVec2(std::floor(size.x), std::floor(size.y));
    dl->AddRectFilled(pos, pos + size, u32(p.ui));
    dl->AddLine(pos, ImVec2(pos.x + size.x, pos.y), u32(p.border), 1.0f);

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##transport", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const bool ready = has_file();
    uint64_t s = 0, span = 1, cur = 0;
    if (ready) {
        uint64_t e = g_pb->end_time_us();
        s = g_pb->start_time_us();
        span = e > s ? e - s : 1;
        cur = std::clamp<uint64_t>(g_pb->current_time_us(), s, s + span);
    }
    float frac = std::clamp((float)((double)(cur - s) / (double)span), 0.0f, 1.0f);
    const bool playing = ready && g_pb->playing();
    const bool ended = ready && !playing && g_pb->current_time_us() + 40'000 >= s + span;

    const ImU32 c_dim = u32(fade(p.text, 0.85f));
    const ImU32 c_lit = u32(p.light);
    const float cy = std::floor(pos.y + size.y * 0.5f);

    // mm:ss.cc of a microsecond duration.
    auto fmt_time = [](char* out, size_t n, uint64_t us) {
        uint64_t sec = us / 1'000'000, cs = (us / 10'000) % 100;
        std::snprintf(out, n, "%llu:%02llu.%02llu", (unsigned long long)(sec / 60),
                      (unsigned long long)(sec % 60), (unsigned long long)cs);
    };

    auto ico_btn = [&](const char* id, const char* icon, const char* tip, float glyph, bool lit,
                       float bx, float box) -> bool {
        ImVec2 bs(box, box);
        ImVec2 bp = snap(ImVec2(bx, cy - box * 0.5f));
        ImGui::PushID(id);
        ImGui::SetCursorScreenPos(bp);
        ImGui::InvisibleButton("b", bs);
        bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
        ImGui::PopID();
        if (hov) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); if (tip) tooltip(tip); }
        icon_centered(dl, icon, bp, bp + bs, glyph, lit ? u32(p.accent) : (hov ? c_lit : c_dim));
        return clk;
    };

    // ── Left: controls ─────────────────────────────────────────────────
    float x = pos.x + 12.0f;

    {
        bool on = settings::get().loop_at_end;
        if (ico_btn("loop", ICON_REPEAT, on ? "Loop: on" : "Loop: off", 18.0f, on, x, 22.0f)) {
            settings::get().loop_at_end = !on;
            settings::save();
        }
        x += 22.0f + 6.0f;
    }

    { // speed pill
        static const float SPEEDS[] = {0.5f, 1.0f, 2.0f, 4.0f};
        char sp[8];
        std::snprintf(sp, sizeof(sp), "%gx", ready ? g_pb->speed() : 1.0f);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float sw = ImGui::CalcTextSize(sp).x, w = sw + 18.0f;
        ImVec2 bp(std::floor(x), cy - 11.0f);
        ImGui::SetCursorScreenPos(bp);
        ImGui::PushID("spd");
        ImGui::InvisibleButton("b", ImVec2(w, 22.0f));
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("m");
        float lh = ImGui::GetTextLineHeight();
        dl->AddText(snap(ImVec2(bp.x, cy - lh * 0.5f)), hov ? c_lit : u32(p.text), sp);
        icon_centered(dl, ICON_CARET_DOWN, ImVec2(bp.x + sw + 1.0f, bp.y),
                      ImVec2(bp.x + sw + 15.0f, bp.y + 22.0f), 14.0f, u32(p.subtle_text));
        ImGui::PushStyleColor(ImGuiCol_PopupBg, u32(p.ui));
        ImGui::PushStyleColor(ImGuiCol_Border, u32(p.border));
        if (ImGui::BeginPopup("m")) {
            for (float v : SPEEDS) {
                char l[8];
                std::snprintf(l, sizeof(l), "%gx", v);
                if (ImGui::Selectable(l, ready && g_pb->speed() == v) && ready) g_pb->set_speed(v);
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::PopFont();
        x = bp.x + w + 6.0f;
    }

    // Episodes are selected from the list in the left dock panel, not here.

    if (ico_btn("first", ICON_SKIP_PREVIOUS, "Jump to start", 23.0f, false, x, 24.0f) && ready)
        g_pb->seek(s);
    x += 24.0f + 2.0f;

    { // play — a filled circle, bigger than the skips
        const float R = 15.0f;
        float ccx = std::floor(x + R), ccy = cy;
        ImVec2 bp = snap(ImVec2(ccx - R, ccy - R));
        ImGui::PushID("play");
        ImGui::SetCursorScreenPos(bp);
        ImGui::InvisibleButton("b", ImVec2(R * 2.0f, R * 2.0f));
        bool hov = ImGui::IsItemHovered(), clk = ImGui::IsItemClicked();
        ImGui::PopID();
        if (hov) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            tooltip(ended ? "Replay" : playing ? "Pause" : "Play");
        }
        dl->AddCircleFilled(ImVec2(ccx, ccy), R, u32(hov ? p.light : mix(p.light, p.ui, 0.14f)), 64);
        // Faux-bold: the Material glyph drawn a few times at sub-pixel offsets.
        {
            const char* g = ended ? ICON_REPLAY : playing ? ICON_PAUSE : ICON_PLAY;
            const float gpx = 20.0f;
            ImGui::PushFont(fonts::body(), gpx);
            ImVec2 ts = ImGui::CalcTextSize(g);
            ImVec2 gp(std::floor(ccx - ts.x * 0.5f + 0.5f),
                      std::floor(ccy - gpx * 0.5f + gpx * 0.10f + 0.5f));
            for (ImVec2 o : {ImVec2(0, 0), ImVec2(0.9f, 0), ImVec2(0, 0.9f), ImVec2(0.9f, 0.9f)})
                dl->AddText(ImVec2(gp.x + o.x, gp.y + o.y), u32(p.frame), g);
            ImGui::PopFont();
        }
        if (clk && ready) {
            if (ended) { g_pb->seek(s); g_pb->play(); }
            else g_pb->toggle();
        }
        x = ccx + R + 2.0f;
    }

    if (ico_btn("last", ICON_SKIP_NEXT, "Jump to end", 23.0f, false, x, 24.0f) && ready)
        g_pb->seek(s + span);
    x += 24.0f + 14.0f;

    // ── time: elapsed (bright) / total (dim) ───────────────────────────
    ImGui::PushFont(nullptr, theme::size::SMALL);
    float lh = ImGui::GetTextLineHeight();
    float ty = std::floor(cy - lh * 0.5f);
    {
        char a[20] = "--", b[20] = "--";
        if (ready) { fmt_time(a, sizeof(a), cur - s); fmt_time(b, sizeof(b), span); }
        char head[24];
        std::snprintf(head, sizeof(head), "%s / ", a);
        dl->AddText(snap(ImVec2(x, ty)), u32(p.text), head);
        float hw = ImGui::CalcTextSize(head).x;
        dl->AddText(snap(ImVec2(x + hw, ty)), u32(p.subtle_text), b);
        char full[48];
        std::snprintf(full, sizeof(full), "%s%s", head, b);
        x += ImGui::CalcTextSize(full).x + 16.0f;
    }

    // ── frame counter, right-aligned ──────────────────────────────────
    float scrub_x1 = pos.x + size.x - 12.0f;
    if (ready && !g_pb->video_topics().empty()) {
        uint64_t total = g_pb->total_message_count(g_pb->video_topics()[0]);
        if (total > 0) {
            uint64_t curf = (uint64_t)(frac * (double)total + 0.5);
            char cf[16], tf[16];
            std::snprintf(cf, sizeof(cf), "%llu ", (unsigned long long)curf);
            std::snprintf(tf, sizeof(tf), "/ %llu", (unsigned long long)total);
            float cfw = ImGui::CalcTextSize(cf).x, tfw = ImGui::CalcTextSize(tf).x;
            float fx = pos.x + size.x - 12.0f - cfw - tfw;
            dl->AddText(snap(ImVec2(fx, ty)), u32(p.text), cf);
            dl->AddText(snap(ImVec2(fx + cfw, ty)), u32(p.subtle_text), tf);
            scrub_x1 = fx - 14.0f;
        }
    }
    ImGui::PopFont();

    // ── Scrubber — fills the space between the time and the frame count ──
    float scrub_x0 = x;
    if (scrub_x1 < scrub_x0 + 40.0f) scrub_x1 = scrub_x0 + 40.0f;
    float sw2 = scrub_x1 - scrub_x0;
    ImGui::SetCursorScreenPos(ImVec2(scrub_x0, cy - 8.0f));
    ImGui::InvisibleButton("##scrub", ImVec2(sw2, 16.0f));
    bool scrub_active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || scrub_active) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    // A scrub pauses and leaves the video parked on the target frame — drag
    // to a spot, that's where it stays. Press play to resume.
    if (ImGui::IsItemActivated() && ready && g_pb->playing()) g_pb->pause();
    // Only send a seek when the target actually moves — holding the handle
    // still would otherwise fire an identical seek every frame and keep the
    // playback thread churning catch-ups that never settle.
    static uint64_t s_last_scrub_target = UINT64_MAX;
    if (scrub_active && ready) {
        float rel = std::clamp((ImGui::GetIO().MousePos.x - scrub_x0) / sw2, 0.0f, 1.0f);
        uint64_t target = s + (uint64_t)(rel * span);
        if (target != s_last_scrub_target) {
            s_last_scrub_target = target;
            g_pb->seek(target);
        }
    } else {
        s_last_scrub_target = UINT64_MAX;
    }
    float px = std::floor(scrub_x0 + sw2 * frac);
    dl->AddRectFilled(ImVec2(scrub_x0, cy - 2.0f), ImVec2(scrub_x1, cy + 2.0f),
                      u32(mix(p.ui, p.deep, 0.5f)), 2.0f);
    if (ready && px > scrub_x0)
        dl->AddRectFilled(ImVec2(scrub_x0, cy - 2.0f), ImVec2(px, cy + 2.0f), u32(p.accent), 2.0f);
    if (ready) {
        ImVec2 m = ImGui::GetIO().MousePos;
        bool head = m.x >= px - 8.0f && m.x <= px + 8.0f && m.y >= cy - 9.0f && m.y <= cy + 9.0f;
        bool shov = head || scrub_active;
        dl->AddCircleFilled(ImVec2(px, cy), shov ? 6.5f : 5.0f, c_lit);
        if (shov) {
            char hb[20];
            fmt_time(hb, sizeof(hb), cur - s);
            ImDrawList* fg = ImGui::GetForegroundDrawList();
            ImGui::PushFont(nullptr, theme::size::SMALL);
            ImVec2 ts = ImGui::CalcTextSize(hb);
            float bx = std::clamp(px, scrub_x0 + ts.x * 0.5f + 8.0f, scrub_x1 - ts.x * 0.5f - 8.0f);
            float bot = pos.y - 3.0f, top = bot - ts.y - 8.0f;
            ImVec2 q0(std::floor(bx - ts.x * 0.5f - 7.0f), std::floor(top));
            ImVec2 q1(std::floor(bx + ts.x * 0.5f + 7.0f), std::floor(bot));
            fg->AddRectFilled(q0, q1, u32(p.bright_ui), 3.0f);
            fg->AddTriangleFilled(ImVec2(px - 4.0f, bot - 0.5f), ImVec2(px + 4.0f, bot - 0.5f),
                                  ImVec2(px, bot + 4.0f), u32(p.bright_ui));
            fg->AddText(snap(ImVec2(bx - ts.x * 0.5f, top + 3.0f)), u32(p.bright_ui_text), hb);
            ImGui::PopFont();
        }
    }

    ImGui::EndChild();
}

// ── Right panel sections ───────────────────────────────────────────────
void topic_list_body() {
    const theme::Palette& p = theme::palette();
    if (!has_file()) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "No file open.");
        ImGui::PopFont();
        return;
    }
    for (const auto& t : g_pb->topics()) {
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%llu", (unsigned long long)g_pb->message_count(t));
        bool sel = (t == g_selected_topic);

        float w = ImGui::GetContentRegionAvail().x;
        ImVec2 rp = ImGui::GetCursorScreenPos();
        ImGui::PushID(t.c_str());
        ImGui::InvisibleButton("row", ImVec2(w, 24.0f));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked()) g_selected_topic = sel ? std::string() : t;
        ImGui::PopID();

        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (sel)
            dl->AddRectFilled(rp, ImVec2(rp.x + w, rp.y + 24.0f), u32(p.selected), 3.0f);
        else if (hov)
            dl->AddRectFilled(rp, ImVec2(rp.x + w, rp.y + 24.0f), u32(p.button), 3.0f);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float th = ImGui::GetTextLineHeight();
        dl->AddText(ImVec2(rp.x + 6, rp.y + (24.0f - th) * 0.5f),
                    u32(sel || hov ? p.light : p.text), t.c_str());
        ImVec2 cs = ImGui::CalcTextSize(cnt);
        dl->AddText(ImVec2(rp.x + w - cs.x - 6, rp.y + (24.0f - th) * 0.5f), u32(p.subtle_text),
                    cnt);
        ImGui::PopFont();
    }
}

void inspector_body() {
    const theme::Palette& p = theme::palette();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    if (!has_file()) {
        ImGui::TextColored(p.subtle_text, "Open a recording, then pick a topic.");
        ImGui::PopFont();
        return;
    }
    if (g_selected_topic.empty()) {
        ImGui::TextColored(p.subtle_text, "Select a topic above.");
        ImGui::PopFont();
        return;
    }
    ImGui::PopFont();
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    ImGui::TextWrapped("%s", g_selected_topic.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 3));
    ImGui::PushFont(nullptr, theme::size::SMALL);
    std::string summary = g_pb->latest_summary(g_selected_topic);
    ImGui::TextColored(p.text, "%s", summary.empty() ? "(no message yet)" : summary.c_str());
    ImGui::Dummy(ImVec2(0, 3));
    char cnt[48];
    std::snprintf(cnt, sizeof(cnt), "%llu messages dispatched",
                  (unsigned long long)g_pb->message_count(g_selected_topic));
    ImGui::TextColored(p.subtle_text, "%s", cnt);
    ImGui::PopFont();
}

// Right-panel wrappers — lay out a rect at the cursor and defer to the
// shared chart. (scalar_chart / audio_chart are the single implementations.)
void imu_plot(const std::string& topic, float height) {
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    scalar_chart(ImGui::GetWindowDrawList(), pos, ImVec2(pos.x + w, pos.y + height), topic,
                 /*inline_legend=*/true, u32(theme::palette().frame));
    ImGui::Dummy(ImVec2(w, height));
}

void audio_panel(float height) {
    const theme::Palette& p = theme::palette();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    audio_chart(dl, pos, ImVec2(pos.x + w, pos.y + height), u32(p.frame));
    ImGui::Dummy(ImVec2(w, height));
}

void sensors_body() {
    const theme::Palette& p = theme::palette();
    if (!has_file()) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "Open a recording.");
        ImGui::PopFont();
        return;
    }
    bool any = false;
    for (const auto& t : g_pb->scalar_topics()) {
        any = true;
        bb::field_label(upper(short_topic(t)).c_str());
        imu_plot(t, 120.0f);
        ImGui::Dummy(ImVec2(0, 8));
    }
    if (g_pb->has_audio()) {
        bb::field_label("AUDIO");
        audio_panel(72.0f);
        any = true;
    }
    if (!any) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "No /imu/* or /audio topics.");
        ImGui::PopFont();
    }
}

// The left dock panel. Sits between the rail and the video stage, toggled by
// the rail's sidebar button, drag-resizable via panel_splitter. Hosts the
// episode list for multi-segment (LeRobot) recordings; otherwise empty
// (device / IMU / colour controls are a later batch).
void side_panel(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, u32(p.ui));
    const float head_h = 34.0f;
    dl->AddLine(ImVec2(pos.x, pos.y + head_h), ImVec2(pos.x + size.x, pos.y + head_h),
                u32(p.border), 1.0f);
    dl->AddLine(ImVec2(pos.x + size.x, pos.y), ImVec2(pos.x + size.x, pos.y + size.y),
                u32(p.border), 1.0f);

    const bool ep_list = has_file() && g_pb->segment_count() > 1;

    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##sidepanel", size, ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);

    if (ep_list) {
        const int n = g_pb->segment_count();
        const int cur = g_pb->current_segment();

        // Header: "EPISODES" + count.
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float lh = ImGui::GetTextLineHeight();
        float hy = std::floor(pos.y + (head_h - lh) * 0.5f);
        dl->AddText(snap(ImVec2(pos.x + 12, hy)), u32(p.subtle_text), "EPISODES");
        char cnt[16];
        std::snprintf(cnt, sizeof(cnt), "%d", n);
        float cw = ImGui::CalcTextSize(cnt).x;
        dl->AddText(snap(ImVec2(pos.x + size.x - 12 - cw, hy)), u32(p.subtle_text), cnt);
        ImGui::PopFont();

        // Search + scrolling list live in a padded inner child.
        ImGui::SetCursorScreenPos(ImVec2(pos.x + 10, pos.y + head_h + 8));
        ImGui::BeginChild("##epbody", ImVec2(size.x - 12, size.y - head_h - 14),
                          ImGuiChildFlags_None);

        bb::search("##epsearch", &g_ep_filter);
        std::string flt = g_ep_filter;
        for (char& c : flt)
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');

        ImGui::Dummy(ImVec2(0, 6));
        ImGui::BeginChild("##eplist", ImVec2(0, 0), ImGuiChildFlags_None);

        const float ROW_H = 46.0f;
        for (int i = 0; i < n; ++i) {
            mp::SegmentInfo si = g_pb->segment_info(i);
            std::string tl = si.task;
            for (char& c : tl)
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "%d", i);
            if (!flt.empty() && tl.find(flt) == std::string::npos &&
                std::string(idbuf).find(flt) == std::string::npos)
                continue;

            ImVec2 rp = ImGui::GetCursorScreenPos();
            float rw = ImGui::GetContentRegionAvail().x;
            ImVec2 rs(rw, ROW_H);
            ImGui::PushID(i);
            ImGui::InvisibleButton("row", rs);
            bool hov = ImGui::IsItemHovered();
            bool clk = ImGui::IsItemClicked();
            ImGui::PopID();
            if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

            const bool sel = i == cur;
            if (sel)
                dl->AddRectFilled(rp, rp + rs, u32(p.selected));
            else if (hov)
                dl->AddRectFilled(rp, rp + rs, u32(mix(p.ui, p.selected, 0.5f)));
            if (sel)
                dl->AddRectFilled(rp, ImVec2(rp.x + 2.0f, rp.y + rs.y), u32(p.accent));

            char num[16];
            std::snprintf(num, sizeof(num), "# %d", i);
            char dur[16] = "";
            if (si.duration_us > 0) {
                uint64_t sec = si.duration_us / 1'000'000;
                std::snprintf(dur, sizeof(dur), "%llu:%02llu", (unsigned long long)(sec / 60),
                              (unsigned long long)(sec % 60));
            }

            ImGui::PushFont(nullptr, theme::size::SMALL);
            float slh = ImGui::GetTextLineHeight();
            dl->AddText(snap(ImVec2(rp.x + 12, rp.y + 7)), u32(p.subtle_text), num);
            if (dur[0]) {
                float dw = ImGui::CalcTextSize(dur).x;
                dl->AddText(snap(ImVec2(rp.x + rw - 12 - dw, rp.y + (ROW_H - slh) * 0.5f)),
                            u32(p.subtle_text), dur);
            }
            ImGui::PopFont();

            const char* label = si.task.empty() ? si.name.c_str() : si.task.c_str();
            ImVec2 tp = snap(ImVec2(rp.x + 12, rp.y + 7 + slh + 3));
            ImVec4 clip(tp.x, tp.y, rp.x + rw - (dur[0] ? 44.0f : 12.0f), tp.y + slh + 4);
            dl->AddText(fonts::body(), theme::size::SMALL, tp,
                        u32(sel || hov ? p.light : p.text), label, nullptr, 0.0f, &clip);

            if (clk) g_pb->select_segment(i);
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    ImGui::EndChild();
}

} // namespace

void init(WGPUDevice device, WGPUQueue queue) {
    g_device = device;
    g_queue = queue;
    g_pb = std::make_unique<mp::Playback>();

#if defined(_WIN32)
    // Warm up the shell file-dialog machinery (windows.storage.dll,
    // explorerframe.dll, the shell namespace) on a background thread so the
    // first Open… click doesn't pay a ~1-2s cold load.
    std::thread([] {
        if (SUCCEEDED(CoInitializeEx(nullptr,
                                     COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
            IFileOpenDialog* dlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&dlg))) &&
                dlg)
                dlg->Release();
            CoUninitialize();
        }
    }).detach();
#endif
}

void shutdown() {
    g_textures.clear();
    g_pb.reset();
}

#if defined(_WIN32)
namespace {
void stash_pick(const std::wstring& wpath) {
    if (wpath.empty()) return;
    int len = WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len > 0 ? len - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wpath.c_str(), -1, utf8.data(), len, nullptr, nullptr);
    std::lock_guard<std::mutex> lk(g_open_mx);
    g_pending_open_path = std::move(utf8);
}
} // namespace
#endif

// The native dialog runs modally on the UI thread (it pops instantly there;
// the window is briefly "not responding" while it's up, which is normal). The
// pick is stashed and the actual open() then runs off-thread via poll_open().
void open_dialog() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"MCAP recordings\0*.mcap\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) stash_pick(path);
#endif
}

void open_folder_dialog() {
#if defined(_WIN32)
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool did_init = init == S_OK || init == S_FALSE;
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg)))) {
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        if (SUCCEEDED(dlg->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = nullptr;
                const SIGDN kFsPath = static_cast<SIGDN>(0x80058000); // SIGDN_FILESYSTEMPATH
                if (SUCCEEDED(item->GetDisplayName(kFsPath, &wpath)) && wpath) {
                    stash_pick(wpath);
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (did_init) CoUninitialize();
#endif
}

void open_path(const char* utf8_path) {
    if (!g_pb || !utf8_path || !*utf8_path) return;
    g_textures.clear();
    g_imu_scale.clear();
    g_axis_hidden.clear();
    g_selected_topic.clear();
    g_view.clear();
    g_sensor_panel.clear();
    g_focus_topic.clear();
    g_featured.clear();
    g_ep_filter.clear();
    g_rotation = settings::get().default_rotation; // panels seed from this
    g_await_post_open = true;
    g_pb->open(utf8_path); // async — returns immediately, is_open() flips when loaded
}

// Consume a path picked by a dialog thread, and finish open-time UI setup once
// the async open lands. Call once per frame.
void poll_open() {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_open_mx);
        if (!g_pending_open_path.empty()) std::swap(path, g_pending_open_path);
    }
    if (!path.empty()) open_path(path.c_str());

    if (g_await_post_open && g_pb && !g_pb->opening()) {
        g_await_post_open = false;
        if (g_pb->is_open()) {
            std::fprintf(stderr, "opened: %s (%zu topics, %zu video)\n", g_pb->path().c_str(),
                         g_pb->topics().size(), g_pb->video_topics().size());
            g_pb->set_speed(settings::get().default_speed);
            if (g_pb->segment_count() > 1) g_panel_hidden = false; // reveal the episode list
            if (settings::get().autoplay_on_open) g_pb->play();
        }
    }
}

bool has_file() { return g_pb && g_pb->is_open(); }
mp::Playback& playback() { return *g_pb; }

// A vertical drag handle on the left panel's right edge. Submitted last (its
// own child window) so it wins input over the video/panel children beneath.
// `edge_x` is the panel's right edge.
void panel_splitter(float edge_x, ImVec2 area_pos, float panel_h) {
    const theme::Palette& p = theme::palette();
    const float grab = 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(edge_x - grab * 0.5f, area_pos.y));
    ImGui::BeginChild("##panelsplit", ImVec2(grab, panel_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::InvisibleButton("h", ImVec2(grab, panel_h));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (act) {
        // Dragging the handle right (mouse dx > 0) widens the panel.
        g_panel_w = std::clamp(g_panel_w + ImGui::GetIO().MouseDelta.x, PANEL_W_MIN, PANEL_W_MAX);
    }
    ImGui::GetWindowDrawList()->AddLine(ImVec2(edge_x, area_pos.y),
                                       ImVec2(edge_x, area_pos.y + panel_h),
                                       u32(hov || act ? p.accent : p.border),
                                       hov || act ? 2.0f : 1.0f);
    ImGui::EndChild();
}

void layout(ImVec2 o, ImVec2 sz) {
    if (sz.x <= 0 || sz.y <= 0) return;

    poll_open(); // consume a dialog pick / finish async-open setup

    // Loop mode: when playback has run to the end, jump back to the start and
    // keep going. (Without loop mode the playback loop just stops there and the
    // transport shows its replay control.)
    if (has_file() && settings::get().loop_at_end) {
        uint64_t e = g_pb->end_time_us(), s = g_pb->start_time_us();
        if (e > s && !g_pb->playing() && g_pb->current_time_us() + 40'000 >= e) {
            g_pb->seek(s);
            g_pb->play();
        }
    }

    float body_w = sz.x - RAIL_W;
    float panel_w = g_panel_hidden ? 0.0f
                                   : std::clamp(g_panel_w, PANEL_W_MIN,
                                                std::max(PANEL_W_MIN, body_w - 200.0f));
    float region_h = sz.y - TRANSPORT_H;

    ImVec2 rail_pos = o;
    ImVec2 rail_sz(RAIL_W, sz.y);

    // Left dock panel spans the full height; the video stage + its transport
    // stack vertically in the column to its right.
    ImVec2 panel_pos(o.x + RAIL_W, o.y);
    ImVec2 panel_sz(panel_w, sz.y);

    float stage_w = std::max(120.0f, body_w - panel_w);
    ImVec2 toolbar_pos(o.x + RAIL_W + panel_w, o.y);
    ImVec2 toolbar_sz(stage_w, TOOLBAR_H);
    ImVec2 disp_pos(o.x + RAIL_W + panel_w, o.y + TOOLBAR_H);
    ImVec2 disp_sz(stage_w, region_h - TOOLBAR_H);

    ImVec2 transport_pos(o.x + RAIL_W + panel_w, o.y + region_h);
    ImVec2 transport_sz(stage_w, TRANSPORT_H);

    preview_toolbar(toolbar_pos, toolbar_sz);
    display(disp_pos, disp_sz);
    if (panel_w > 0.0f) side_panel(panel_pos, panel_sz);
    transport(transport_pos, transport_sz);
    rail(rail_pos, rail_sz);
    if (panel_w > 0.0f) panel_splitter(panel_pos.x + panel_w, o, sz.y);
}

} // namespace player_ui
