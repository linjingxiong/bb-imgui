#include "mcap_ui.h"

#include "bb.h"
#include "fonts.h"
#include "icons.h"
#include "playback.h"
#include "theme.h"
#include "video_texture.h"

#include "imgui.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX // keep the windows.h min/max macros from breaking std::min({...})
#endif
#include <windows.h>
#include <commdlg.h>
#endif

namespace mcap_ui {
namespace {

WGPUDevice g_device = nullptr;
WGPUQueue g_queue = nullptr;
std::unique_ptr<mp::Playback> g_pb;
std::map<std::string, std::unique_ptr<mp::VideoTexture>> g_textures;
std::string g_selected_topic;
int g_rotation = 90; // degrees CW — the Ego device's cameras are mounted sideways
// Adaptive per-topic IMU Y-scale — grows to the largest |value| seen, never
// shrinks (EgoViewer's accMaxSeen_/gyroMaxSeen_). Reset on file open.
std::map<std::string, float> g_imu_scale;

// EgoViewer SensorPanel axis colours (softer than theme::axis).
const ImVec4 kAxisR = ImVec4(0xEF / 255.0f, 0x44 / 255.0f, 0x44 / 255.0f, 1.0f);
const ImVec4 kAxisG = ImVec4(0x22 / 255.0f, 0xC5 / 255.0f, 0x5E / 255.0f, 1.0f);
const ImVec4 kAxisB = ImVec4(0x3B / 255.0f, 0x82 / 255.0f, 0xF6 / 255.0f, 1.0f);

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

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

void fmt_time(char* buf, size_t n, uint64_t us) {
    uint64_t total_ms = us / 1000;
    uint64_t ms = total_ms % 1000;
    uint64_t s = (total_ms / 1000) % 60;
    uint64_t m = (total_ms / 60000) % 60;
    uint64_t h = total_ms / 3600000;
    if (h > 0) std::snprintf(buf, n, "%llu:%02llu:%02llu.%03llu",
                             (unsigned long long)h, (unsigned long long)m,
                             (unsigned long long)s, (unsigned long long)ms);
    else std::snprintf(buf, n, "%02llu:%02llu.%03llu", (unsigned long long)m,
                       (unsigned long long)s, (unsigned long long)ms);
}

const char* short_topic(const std::string& t) {
    auto pos = t.find_last_of('/');
    return pos == std::string::npos ? t.c_str() : t.c_str() + pos + 1;
}

} // namespace

void init(WGPUDevice device, WGPUQueue queue) {
    g_device = device;
    g_queue = queue;
    g_pb = std::make_unique<mp::Playback>();
}

void shutdown() {
    g_textures.clear();
    g_pb.reset();
}

void open_dialog() {
#if defined(_WIN32)
    wchar_t path[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"MCAP recordings\0*.mcap\0All files\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return;

    int len = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(len > 0 ? len - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8.data(), len, nullptr, nullptr);

    open_path(utf8.c_str());
#endif
}

void open_path(const char* utf8_path) {
    if (!g_pb || !utf8_path || !*utf8_path) return;
    g_textures.clear();
    g_imu_scale.clear();
    g_selected_topic.clear();
    if (g_pb->open(utf8_path))
        std::fprintf(stderr, "mcap: opened %s (%zu topics, %zu video)\n", utf8_path,
                     g_pb->topics().size(), g_pb->video_topics().size());
    else
        std::fprintf(stderr, "mcap: failed to open %s\n", utf8_path);
}

bool has_file() { return g_pb && g_pb->is_open(); }
mp::Playback& playback() { return *g_pb; }

void topic_tree() {
    const theme::Palette& p = theme::palette();
    bb::field_label("TOPICS");
    ImGui::Dummy(ImVec2(0, 4));

    if (!has_file()) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "No file open.");
        ImGui::Dummy(ImVec2(0, 6));
        if (bb::button("Open MCAP\xe2\x80\xa6")) open_dialog();
        ImGui::PopFont();
        return;
    }

    for (const auto& t : g_pb->topics()) {
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%llu", (unsigned long long)g_pb->message_count(t));
        bool sel = (t == g_selected_topic);
        if (bb::outliner_node(t.c_str(), /*leaf=*/true, sel, nullptr, t.c_str()))
            g_selected_topic = t;
        // outliner_node returns true only for non-leaf toggles; catch the
        // click on a leaf row via the last item.
        if (ImGui::IsItemClicked()) g_selected_topic = t;
        ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
        ImVec2 ts = ImGui::CalcTextSize(cnt);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rmax.x - ts.x - 24.0f, rmin.y + (rmax.y - rmin.y - ts.y) * 0.5f),
            u32(p.subtle_text), cnt);
        ImGui::PopFont();
    }
}

void inspector() {
    const theme::Palette& p = theme::palette();
    bb::field_label("INSPECTOR");
    ImGui::Dummy(ImVec2(0, 4));
    if (!has_file()) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "Open a recording, then pick a topic.");
        ImGui::PopFont();
        return;
    }
    if (g_selected_topic.empty()) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "Select a topic on the left.");
        ImGui::PopFont();
        return;
    }
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    ImGui::TextUnformatted(g_selected_topic.c_str());
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushFont(nullptr, theme::size::SMALL);
    std::string summary = g_pb->latest_summary(g_selected_topic);
    ImGui::TextColored(p.text, "%s", summary.empty() ? "(no message yet)" : summary.c_str());
    ImGui::Dummy(ImVec2(0, 4));
    char cnt[48];
    std::snprintf(cnt, sizeof(cnt), "%llu messages dispatched",
                  (unsigned long long)g_pb->message_count(g_selected_topic));
    ImGui::TextColored(p.subtle_text, "%s", cnt);
    ImGui::PopFont();
}

// An x/y/z chart for one IMU topic, modelled on EgoViewer's
// SensorPanel::drawAxisLegendChart: a 5s window, RAW values (no detrend)
// against an adaptive-max symmetric Y scale, a faint grid with numeric Y
// labels, a bottom-left colour/letter/value legend, and an "Acc"/"Gyro"
// tag bottom-right.
void imu_plot(const std::string& topic, float height) {
    const theme::Palette& p = theme::palette();
    auto hist = g_pb->imu_history(topic);
    auto latest = g_pb->imu_latest(topic);
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();

    const uint64_t now = g_pb->current_time_us();
    const uint64_t window_us = 5'000'000;
    const uint64_t win_start = now > window_us ? now - window_us : 0;

    const bool is_gyro = topic.find("gyro") != std::string::npos;
    float& scale = g_imu_scale[topic];
    if (scale <= 0.0f) scale = is_gyro ? 1.0f : 15.0f;
    for (const auto& s : hist) {
        if (s.t_us < win_start || s.t_us > now) continue;
        scale = std::max({scale, (float)std::abs(s.x), (float)std::abs(s.y),
                          (float)std::abs(s.z)});
    }

    // Chart rect — a left margin for the Y labels, small padding elsewhere.
    const float y_label_w = 30.0f;
    ImVec2 c0(pos.x + y_label_w, pos.y + 4.0f);
    ImVec2 c1(pos.x + w, pos.y + height - 4.0f);
    dl->AddRectFilled(c0, c1, u32(p.deep));

    const ImU32 grid = IM_COL32(255, 255, 255, 22);
    for (int i = 0; i <= 5; ++i) {
        float gx = c0.x + (c1.x - c0.x) * i / 5.0f;
        dl->AddLine(ImVec2(gx, c0.y), ImVec2(gx, c1.y), grid, 1.0f);
    }
    ImGui::PushFont(nullptr, theme::size::SMALL * 0.8f);
    int dec = scale >= 10 ? 0 : (scale >= 1 ? 1 : 2);
    for (int i = 0; i <= 4; ++i) {
        float frac = 1.0f - i / 2.0f; // +1, +0.5, 0, -0.5, -1
        float gy = c0.y + (c1.y - c0.y) * i / 4.0f;
        dl->AddLine(ImVec2(c0.x, gy), ImVec2(c1.x, gy), grid, 1.0f);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.*f", dec, frac * scale);
        ImVec2 ts = ImGui::CalcTextSize(lbl);
        dl->AddText(ImVec2(pos.x + y_label_w - 4 - ts.x, gy - ts.y * 0.5f), u32(p.subtle_text),
                    lbl);
    }
    ImGui::PopFont();

    auto X = [&](uint64_t t) {
        return c1.x - (float)((double)(now - t) / (double)window_us) * (c1.x - c0.x);
    };
    float mid = (c0.y + c1.y) * 0.5f;
    auto Y = [&](double v) {
        double norm = std::clamp(v / scale, -1.0, 1.0);
        return mid - (float)norm * (c1.y - c0.y) * 0.48f;
    };

    const ImVec4 acol[3] = {kAxisR, kAxisG, kAxisB};
    dl->PushClipRect(c0, c1, true);
    for (int axis = 0; axis < 3; ++axis) {
        ImU32 col = u32(ImVec4(acol[axis].x, acol[axis].y, acol[axis].z, 0.9f));
        bool have_prev = false;
        ImVec2 prev;
        for (const auto& s : hist) {
            if (s.t_us < win_start || s.t_us > now) { have_prev = false; continue; }
            double raw = axis == 0 ? s.x : axis == 1 ? s.y : s.z;
            ImVec2 pt(X(s.t_us), Y(raw));
            if (have_prev) dl->AddLine(prev, pt, col, 1.0f);
            prev = pt;
            have_prev = true;
        }
    }
    dl->PopClipRect();

    // Bottom-left legend: swatch + letter + current value, per axis.
    ImGui::PushFont(nullptr, theme::size::SMALL * 0.85f);
    static const char* names[3] = {"x", "y", "z"};
    double vals[3] = {latest.x, latest.y, latest.z};
    float ly = c1.y - 4.0f - 14.0f * 3;
    for (int axis = 0; axis < 3; ++axis) {
        dl->AddCircleFilled(ImVec2(c0.x + 8, ly + 6), 3.5f, u32(acol[axis]));
        char t[40];
        std::snprintf(t, sizeof(t), "%s  % .*f", names[axis], scale >= 10 ? 2 : 3, vals[axis]);
        dl->AddText(ImVec2(c0.x + 16, ly), u32(acol[axis]), t);
        ly += 14.0f;
    }
    // Section tag bottom-right.
    const char* tag = is_gyro ? "Gyro" : "Acc";
    ImVec2 tts = ImGui::CalcTextSize(tag);
    dl->AddText(ImVec2(c1.x - tts.x - 6, c1.y - tts.y - 4), u32(p.subtle_text), tag);
    ImGui::PopFont();

    ImGui::Dummy(ImVec2(w, height));
}

void audio_panel(float height) {
    const theme::Palette& p = theme::palette();
    auto hist = g_pb->audio_history();
    ImVec2 pos = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + height), u32(p.deep), theme::RADIUS);
    dl->AddRect(pos, ImVec2(pos.x + w, pos.y + height), u32(p.border), theme::RADIUS);
    ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
    dl->AddText(ImVec2(pos.x + 6, pos.y + 4), u32(p.subtle_text), "audio");
    ImGui::PopFont();

    const uint64_t now = g_pb->current_time_us();
    const uint64_t window_us = 5'000'000;
    const uint64_t win_start = now > window_us ? now - window_us : 0;
    float cy = pos.y + height * 0.5f + 6.0f;
    for (const auto& a : hist) {
        if (a.t_us < win_start || a.t_us > now) continue;
        float x = pos.x + w * (float)((double)(a.t_us - win_start) / (double)window_us);
        float h = a.amp * (height * 0.5f - 12.0f);
        dl->AddLine(ImVec2(x, cy - h), ImVec2(x, cy + h), u32(p.accent), 1.0f);
    }
    dl->AddLine(ImVec2(pos.x + w - 1, pos.y), ImVec2(pos.x + w - 1, pos.y + height),
                u32(p.subtle_text), 1.0f);
    ImGui::Dummy(ImVec2(w, height));
}

void imu_plots() {
    if (!has_file()) return;
    for (const auto& t : g_pb->topics()) {
        if (t.rfind("/imu/", 0) != 0) continue;
        imu_plot(t, 124.0f);
        ImGui::Dummy(ImVec2(0, 6));
    }
    if (g_pb->has_audio()) {
        audio_panel(80.0f);
        ImGui::Dummy(ImVec2(0, 6));
    }
}

void video_grid() {
    if (!has_file()) {
        const theme::Palette& p = theme::palette();
        ImGui::PushFont(fonts::medium(), theme::size::HEADING);
        ImGui::TextColored(p.subtle_text, "MCAP Player");
        ImGui::PopFont();
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text,
                           "File \xe2\x80\xba Open Model\xe2\x80\xa6 (or the topic panel button) "
                           "to open a .mcap recording.");
        ImGui::PopFont();
        return;
    }

    const auto& vts = g_pb->video_topics();
    if (vts.empty()) {
        ImGui::TextDisabled("This recording has no /camera/* video topics.");
        return;
    }

    // Simple responsive grid: 1 col for 1 stream, else 2 cols.
    int cols = vts.size() == 1 ? 1 : 2;
    float avail = ImGui::GetContentRegionAvail().x;
    float spacing = 6.0f;
    float cell_w = (avail - spacing * (cols - 1)) / cols;

    bool rot90 = (((g_rotation % 360) + 360) % 360) % 180 != 0;

    for (size_t i = 0; i < vts.size(); ++i) {
        const std::string& topic = vts[i];
        if (i % cols != 0) ImGui::SameLine(0, spacing);

        auto& tex = g_textures[topic];
        if (!tex) tex = std::make_unique<mp::VideoTexture>(g_device, g_queue);
        if (auto frame = g_pb->latest_frame(topic)) tex->update(frame);

        // Aspect ratio follows the *displayed* orientation.
        float disp_w = rot90 ? (float)tex->height() : (float)tex->width();
        float disp_h = rot90 ? (float)tex->width() : (float)tex->height();
        float cell_h = tex->valid() && disp_w > 0 ? cell_w * disp_h / disp_w
                                                  : cell_w * 9.0f / 16.0f;

        ImGui::BeginChild((topic + "##vid").c_str(), ImVec2(cell_w, cell_h + 22.0f),
                          ImGuiChildFlags_Borders);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextUnformatted(short_topic(topic));
        ImGui::PopFont();
        if (tex->valid())
            draw_video(ImVec2(cell_w - 4.0f, cell_h), tex->id(), g_rotation);
        else
            ImGui::Dummy(ImVec2(cell_w - 4.0f, cell_h));
        ImGui::EndChild();
    }
}

void timeline() {
    const theme::Palette& p = theme::palette();
    if (!has_file()) return;

    mp::Playback& pb = *g_pb;
    uint64_t start = pb.start_time_us(), end = pb.end_time_us();
    uint64_t cur = pb.current_time_us();
    uint64_t span = end > start ? end - start : 1;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, p.back);
    ImGui::BeginChild("##timeline", ImVec2(0, 40), ImGuiChildFlags_None);
    ImGui::SetCursorPos(ImVec2(8, 8));

    if (bb::icon_button(pb.playing() ? ICON_PAUSE : ICON_PLAY)) pb.toggle();
    ImGui::SameLine(0, 8);

    char t_cur[32], t_end[32];
    fmt_time(t_cur, sizeof(t_cur), cur - start);
    fmt_time(t_end, sizeof(t_end), end - start);

    ImGui::AlignTextToFramePadding();
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::TextColored(p.text, "%s", t_cur);
    ImGui::PopFont();
    ImGui::SameLine(0, 10);

    float bar_w = ImGui::GetContentRegionAvail().x - 90.0f;
    ImVec2 bp = ImGui::GetCursorScreenPos();
    bp.y += 6.0f;
    float bar_h = 6.0f;
    ImGui::InvisibleButton("##scrub", ImVec2(bar_w, 20.0f));
    bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive()) {
        float rel = (ImGui::GetIO().MousePos.x - bp.x) / bar_w;
        rel = rel < 0 ? 0 : (rel > 1 ? 1 : rel);
        pb.seek(start + (uint64_t)(rel * span));
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(bp, ImVec2(bp.x + bar_w, bp.y + bar_h), u32(p.deep), 3.0f);
    float frac = span ? (float)(cur - start) / (float)span : 0.0f;
    dl->AddRectFilled(bp, ImVec2(bp.x + bar_w * frac, bp.y + bar_h), u32(p.accent), 3.0f);
    dl->AddCircleFilled(ImVec2(bp.x + bar_w * frac, bp.y + bar_h * 0.5f),
                        hovered ? 6.0f : 4.5f, u32(p.light));

    ImGui::SameLine(0, 10);
    ImGui::PushFont(nullptr, theme::size::SMALL);
    ImGui::TextColored(p.subtle_text, "%s", t_end);
    ImGui::PopFont();

    ImGui::SameLine(0, 12);
    if (bb::icon_button(ICON_ROTATE)) g_rotation = (g_rotation + 90) % 360;

    ImGui::SameLine(0, 4);
    static const float SPEEDS[] = {0.5f, 1.0f, 2.0f, 4.0f};
    char sp[8];
    std::snprintf(sp, sizeof(sp), "%gx", pb.speed());
    if (bb::button(sp)) {
        int i = 0;
        for (; i < 4; ++i) if (SPEEDS[i] == pb.speed()) break;
        pb.set_speed(SPEEDS[(i + 1) % 4]);
    }

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace mcap_ui
