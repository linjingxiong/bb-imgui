#include "mcap_ui.h"

#include "bb.h"
#include "fonts.h"
#include "icons.h"
#include "playback.h"
#include "settings.h"
#include "theme.h"
#include "video_texture.h"

#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
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

// Per-video-panel view state + the one panel (if any) expanded to fill the
// stage. Reset on file open.
struct View {
    int rot = -1;  // -1 => seed from settings on first use
    int fit = -1;  // -1 => seed; then 0 = contain (letterbox), 1 = cover (fill)
};
std::map<std::string, View> g_view;
std::string g_focus_topic;
bool g_abs_time = true; // transport: show wall-clock timestamp vs M:SS elapsed

void rotate_all() {
    g_rotation = (g_rotation + 90) % 360;
    for (auto& kv : g_view)
        kv.second.rot = ((((kv.second.rot % 360) + 360) % 360) + 90) % 360;
}

// Layout metrics. The right panel width is user-draggable.
constexpr float RAIL_W = 48.0f;
constexpr float TRANSPORT_H = 54.0f;
constexpr float PANEL_W_MIN = 260.0f;
constexpr float PANEL_W_MAX = 640.0f;
float g_panel_w = 324.0f;

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
                   ImU32 col) {
    ImGui::PushFont(fonts::body(), px);
    ImVec2 ts = ImGui::CalcTextSize(glyph);
    float cx = (box_min.x + box_max.x) * 0.5f;
    float cy = (box_min.y + box_max.y) * 0.5f;
    dl->AddText(ImVec2(std::floor(cx - ts.x * 0.5f + 0.5f),
                       std::floor(cy - px * 0.5f + px * 0.10f + 0.5f)),
                col, glyph);
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
        g_view.clear();
        g_focus_topic.clear();
    }
    rail_sep();
    if (rail_btn(ICON_FOLDER_OPEN, "Open MCAP\xe2\x80\xa6", false)) open_dialog();
    if (rail_btn(ICON_ROTATE, "Rotate video 90\xc2\xb0", false)) rotate_all();
    if (rail_btn(ICON_TIMELINE, "Sensors", false)) {}

    // Bottom: Settings, held off the rail's bottom edge.
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + size.y - BTN_H - 12.0f));
    if (rail_btn(ICON_SETTINGS, "Settings", settings::is_open())) settings::open();

    ImGui::EndChild();
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

    bool focused = (g_focus_topic == topic);

    // Title — uppercase, muted (Blockbench panel_handle > label, 1.1em).
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    dl->AddText(snap(ImVec2(pos.x + 10, pos.y + (HEAD - ImGui::GetTextLineHeight()) * 0.5f + 1.0f)),
                u32(p.subtle_text), upper(topic).c_str());
    ImGui::PopFont();

    // Controls — always shown; Blockbench .panel_control brightens on hover
    // (opacity only, no background) and surfaces a tooltip.
    ImGui::PushID((topic + "vp").c_str());
    const float ICON = 18.0f;
    float rx = pos.x + size.x - 4.0f;
    auto hdr_btn = [&](const char* tag, const char* icon, const char* tip, bool active) -> bool {
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
        icon_centered(dl, icon, bp, bp + bs, ICON, u32(c));
        rx = bp.x;
        return clk;
    };

    if (hdr_btn("more", ICON_MORE_VERT, "Panel menu", ImGui::IsPopupOpen("vset")))
        ImGui::OpenPopup("vset");
    if (hdr_btn("set", ICON_SETTINGS, "Panel settings", ImGui::IsPopupOpen("vset")))
        ImGui::OpenPopup("vset");
    if (hdr_btn("exp", focused ? ICON_CLOSE_FULLSCREEN : ICON_OPEN_IN_FULL,
                focused ? "Exit fullscreen" : "Fullscreen", focused))
        g_focus_topic = focused ? std::string() : topic;

    ImGui::SetNextWindowPos(ImVec2(pos.x + size.x - 6.0f, pos.y + HEAD + 4.0f), ImGuiCond_Always,
                            ImVec2(1.0f, 0.0f));
    if (ImGui::BeginPopup("vset")) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::Dummy(ImVec2(196.0f, 0.0f)); // establish a stable popup width for segmented()
        ImGui::TextColored(p.subtle_text, "ROTATION");
        static const char* ROT[] = {"0\xc2\xb0", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
        int ri = (((v.rot % 360) + 360) % 360) / 90;
        if (bb::segmented("rot", &ri, ROT, 4)) v.rot = ri * 90;
        ImGui::Dummy(ImVec2(0, 6));
        ImGui::TextColored(p.subtle_text, "FIT");
        static const char* FIT[] = {"Contain", "Cover"};
        bb::segmented("fit", &v.fit, FIT, 2);
        ImGui::PopFont();
        ImGui::EndPopup();
    }
    ImGui::PopID();

    // Content: the frame, near-flush to the panel body.
    ImVec2 c0(pos.x + 2.0f, pos.y + HEAD + 2.0f);
    ImVec2 csz(size.x - 4.0f, size.y - HEAD - 4.0f);
    if (csz.x < 4.0f || csz.y < 4.0f) return;
    dl->PushClipRect(c0, ImVec2(c0.x + csz.x, c0.y + csz.y), true);

    auto& tex = g_textures[topic];
    if (!tex) tex = std::make_unique<mp::VideoTexture>(g_device, g_queue);
    if (auto frame = g_pb->latest_frame(topic)) tex->update(frame);

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
    }
    dl->PopClipRect();
}

// ── Centre video stage ─────────────────────────────────────────────────
void display(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImGui::SetCursorScreenPos(pos);
    ImGui::BeginChild("##display", size, ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, u32(p.deep)); // Blockbench --color-dark stage

    const bool ready = has_file() && !g_pb->video_topics().empty();
    if (!ready) {
        const char* line1 = has_file() ? "This recording has no /camera/* video." : "No recording open";
        const char* line2 = has_file() ? "" : "Open a .mcap from the rail on the left.";
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

    bool focus_valid =
        !g_focus_topic.empty() &&
        std::find(vts.begin(), vts.end(), g_focus_topic) != vts.end();
    if (focus_valid) {
        video_panel(g_focus_topic, pos, size);
        ImGui::EndChild();
        return;
    }

    // Aspect-fit auto grid: pick the column count that lets each video (plus
    // its header) be the largest, size every card to that, then centre the
    // block — and each ragged row — on the dark stage.
    int n = (int)vts.size();
    const float HEAD = 32.0f, PAD = 4.0f, MARGIN = 12.0f, GAP = 8.0f;

    // Representative aspect in display orientation (falls back before the
    // first frame decodes: portrait if the default rotation is 90/270).
    float aw = 16.0f, ah = 9.0f;
    {
        const std::string& t0 = vts[0];
        int drot = (g_view.count(t0) && g_view[t0].rot >= 0) ? g_view[t0].rot
                                                             : settings::get().default_rotation;
        bool rot90 = ((((drot % 360) + 360) % 360) % 180) != 0;
        auto it = g_textures.find(t0);
        if (it != g_textures.end() && it->second && it->second->valid() &&
            it->second->width() > 0 && it->second->height() > 0) {
            float w = (float)it->second->width(), h = (float)it->second->height();
            aw = rot90 ? h : w;
            ah = rot90 ? w : h;
        } else if (rot90) {
            aw = 9.0f;
            ah = 16.0f;
        }
    }

    float W = size.x - 2 * MARGIN, H = size.y - 2 * MARGIN;
    int cols = 1;
    float best = 0.0f;
    for (int c = 1; c <= n; ++c) {
        int r = (n + c - 1) / c;
        float vidW = (W - (c - 1) * GAP) / c - PAD;
        float vidH = (H - (r - 1) * GAP) / r - HEAD - PAD;
        if (vidW <= 4.0f || vidH <= 4.0f) continue;
        float sc = std::min(vidW / aw, vidH / ah);
        if (sc > best) { best = sc; cols = c; }
    }
    int rows = (n + cols - 1) / cols;
    if (best <= 0.0f) best = std::min((W / cols) / aw, (H / rows) / ah);

    float cardW = std::floor(aw * best + PAD);
    float cardH = std::floor(ah * best + HEAD + PAD);
    float blockH = rows * cardH + (rows - 1) * GAP;
    float y0 = std::floor(pos.y + (size.y - blockH) * 0.5f);

    for (int i = 0; i < n; ++i) {
        int gx = i % cols, gy = i / cols;
        int in_row = (gy == rows - 1) ? (n - gy * cols) : cols;
        float row_w = in_row * cardW + (in_row - 1) * GAP;
        float rx = std::floor(pos.x + (size.x - row_w) * 0.5f);
        video_panel(vts[i], ImVec2(rx + gx * (cardW + GAP), y0 + gy * (cardH + GAP)),
                    ImVec2(cardW, cardH));
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
    float frac = std::clamp((float)(cur - s) / (float)span, 0.0f, 1.0f);
    const bool playing = ready && g_pb->playing();

    ImU32 c_dim = u32(ImVec4(p.text.x, p.text.y, p.text.z, 0.85f));
    ImU32 c_lit = u32(p.light);

    // ── Scrubber — a thin grey track spanning the full width ────────────
    const float trk_cy = std::floor(pos.y + 8.0f);
    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y));
    ImGui::InvisibleButton("##scrub", ImVec2(size.x, 16.0f));
    bool shov = ImGui::IsItemHovered() || ImGui::IsItemActive();
    if (shov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    if (ImGui::IsItemActive() && ready) {
        float rel = std::clamp((ImGui::GetIO().MousePos.x - pos.x) / size.x, 0.0f, 1.0f);
        g_pb->seek(s + (uint64_t)(rel * span));
    }
    const float hh = shov ? 3.0f : 2.0f;
    float px = std::floor(pos.x + size.x * frac);
    dl->AddRectFilled(ImVec2(pos.x, trk_cy - hh), ImVec2(pos.x + size.x, trk_cy + hh),
                      u32(mix(p.ui, p.text, 0.20f)));
    if (ready && px > pos.x)
        dl->AddRectFilled(ImVec2(pos.x, trk_cy - hh), ImVec2(px, trk_cy + hh),
                          u32(mix(p.ui, p.text, 0.42f)));
    if (ready) {
        // Playhead — a small vertical bar riding the track.
        float bw = 3.0f, bhh = shov ? 8.0f : 7.0f;
        dl->AddRectFilled(ImVec2(px - bw * 0.5f, trk_cy - bhh), ImVec2(px + bw * 0.5f, trk_cy + bhh),
                          c_lit, 1.0f);
    }

    // ── Controls row ───────────────────────────────────────────────────
    const float row_y = pos.y + 34.0f;

    auto ico_btn = [&](const char* id, const char* icon, const char* tip, float glyph, bool lit,
                       ImVec2 c) -> bool {
        ImVec2 bs(26, 26);
        ImVec2 bp = snap(ImVec2(c.x - bs.x * 0.5f, c.y - bs.y * 0.5f));
        ImGui::PushID(id);
        ImGui::SetCursorScreenPos(bp);
        ImGui::InvisibleButton("b", bs);
        bool hov = ImGui::IsItemHovered();
        bool clk = ImGui::IsItemClicked();
        ImGui::PopID();
        if (hov) { ImGui::SetMouseCursor(ImGuiMouseCursor_Hand); if (tip) tooltip(tip); }
        icon_centered(dl, icon, bp, bp + bs, glyph,
                      lit ? u32(p.accent) : (hov ? c_lit : c_dim));
        return clk;
    };

    // Left: info toggle + timestamp.
    if (ico_btn("info", ICON_INFO, g_abs_time ? "Show elapsed time" : "Show wall-clock time", 19.0f,
                false, ImVec2(pos.x + 20.0f, row_y)))
        g_abs_time = !g_abs_time;

    char stamp[48];
    if (!ready) {
        std::snprintf(stamp, sizeof(stamp), "--");
    } else if (g_abs_time) {
        std::time_t tt = (std::time_t)(cur / 1'000'000);
        unsigned ms = (unsigned)((cur / 1000) % 1000);
        std::tm tmv{};
#if defined(_WIN32)
        localtime_s(&tmv, &tt);
#else
        localtime_r(&tt, &tmv);
#endif
        char base[32];
        std::strftime(base, sizeof(base), "%Y-%m-%d  %H:%M:%S", &tmv);
        std::snprintf(stamp, sizeof(stamp), "%s.%03u", base, ms);
    } else {
        uint64_t el = (cur - s) / 1'000'000, to = span / 1'000'000;
        std::snprintf(stamp, sizeof(stamp), "%llu:%02llu / %llu:%02llu",
                      (unsigned long long)(el / 60), (unsigned long long)(el % 60),
                      (unsigned long long)(to / 60), (unsigned long long)(to % 60));
    }
    ImGui::PushFont(nullptr, theme::size::SMALL);
    float th = ImGui::GetTextLineHeight();
    dl->AddText(snap(ImVec2(pos.x + 38.0f, row_y - th * 0.5f)), u32(p.text), stamp);
    ImGui::PopFont();

    // Centre: skip-start / play / skip-end.
    float mid = pos.x + size.x * 0.5f;
    if (ico_btn("first", ICON_SKIP_PREVIOUS, "Jump to start", 21.0f, false,
                ImVec2(mid - 34.0f, row_y)) && ready)
        g_pb->seek(s);
    if (ico_btn("play", playing ? ICON_PAUSE : ICON_PLAY, playing ? "Pause" : "Play", 25.0f, false,
                ImVec2(mid, row_y)) && ready)
        g_pb->toggle();
    if (ico_btn("last", ICON_SKIP_NEXT, "Jump to end", 21.0f, false,
                ImVec2(mid + 34.0f, row_y)) && ready)
        g_pb->seek(s + span);

    // Right: loop toggle + speed menu.
    float right = pos.x + size.x - 12.0f;
    {
        static const float SPEEDS[] = {0.5f, 1.0f, 2.0f, 4.0f};
        char sp[8];
        std::snprintf(sp, sizeof(sp), "%gx", ready ? g_pb->speed() : 1.0f);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        float sw = ImGui::CalcTextSize(sp).x;
        float chev = 16.0f;
        float w = sw + chev + 8.0f;
        ImVec2 bp(right - w, row_y - 12.0f);
        ImGui::SetCursorScreenPos(bp);
        ImGui::PushID("spd");
        ImGui::InvisibleButton("b", ImVec2(w, 24.0f));
        bool hov = ImGui::IsItemHovered();
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        if (ImGui::IsItemClicked()) ImGui::OpenPopup("m");
        dl->AddText(snap(ImVec2(bp.x, row_y - th * 0.5f)), hov ? c_lit : u32(p.text), sp);
        icon_centered(dl, ICON_CARET_DOWN, ImVec2(bp.x + sw + 2.0f, bp.y),
                      ImVec2(bp.x + sw + 2.0f + chev, bp.y + 24.0f), 16.0f, u32(p.subtle_text));
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
        right = bp.x - 10.0f;
    }
    {
        bool on = settings::get().loop_at_end;
        if (ico_btn("loop", ICON_REPEAT, on ? "Loop: on" : "Loop: off", 19.0f, on,
                    ImVec2(right - 13.0f, row_y))) {
            settings::get().loop_at_end = !on;
            settings::save();
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

// An x/y/z chart for one IMU topic, modelled on EgoViewer's
// SensorPanel::drawAxisLegendChart (5s window, RAW values, adaptive
// symmetric Y scale, faint grid, colour/letter/value legend, "Acc"/"Gyro").
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

    const float y_label_w = 30.0f;
    ImVec2 c0(pos.x + y_label_w, pos.y + 4.0f);
    ImVec2 c1(pos.x + w, pos.y + height - 4.0f);
    dl->AddRectFilled(c0, c1, u32(p.deep));

    const ImU32 grid = u32(p.grid);
    for (int i = 0; i <= 5; ++i) {
        float gx = c0.x + (c1.x - c0.x) * i / 5.0f;
        dl->AddLine(ImVec2(gx, c0.y), ImVec2(gx, c1.y), grid, 1.0f);
    }
    ImGui::PushFont(nullptr, theme::size::CAPTION);
    int dec = scale >= 10 ? 0 : (scale >= 1 ? 1 : 2);
    for (int i = 0; i <= 4; ++i) {
        float f = 1.0f - i / 2.0f;
        float gy = c0.y + (c1.y - c0.y) * i / 4.0f;
        dl->AddLine(ImVec2(c0.x, gy), ImVec2(c1.x, gy), grid, 1.0f);
        char lbl[16];
        std::snprintf(lbl, sizeof(lbl), "%.*f", dec, f * scale);
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

    ImGui::PushFont(nullptr, theme::size::CAPTION);
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
    ImGui::PushFont(fonts::medium(), theme::size::CAPTION);
    dl->AddText(ImVec2(pos.x + 6, pos.y + 3), u32(p.subtle_text), "AUDIO");
    ImGui::PopFont();

    const uint64_t now = g_pb->current_time_us();
    const uint64_t window_us = 5'000'000;
    const uint64_t win_start = now > window_us ? now - window_us : 0;
    const float cy = pos.y + height * 0.5f + 5.0f;
    const float amp_h = height * 0.5f - 12.0f;

    // Bucket samples into ~3px columns and draw a peak bar per column so a
    // dense recording reads as an envelope, not a solid block.
    const int cols = std::max(1, (int)(w / 3.0f));
    std::vector<float> peak(cols, 0.0f);
    for (const auto& a : hist) {
        if (a.t_us < win_start || a.t_us > now) continue;
        int col = (int)((double)(a.t_us - win_start) / (double)window_us * cols);
        col = std::clamp(col, 0, cols - 1);
        peak[col] = std::max(peak[col], std::min(a.amp, 1.0f));
    }
    ImU32 wav = u32(mix(p.deep, p.subtle_text, 0.7f));
    for (int i = 0; i < cols; ++i) {
        if (peak[i] <= 0.0f) continue;
        float x = pos.x + (i + 0.5f) * (w / cols);
        float h = std::max(0.5f, peak[i] * amp_h);
        dl->AddLine(ImVec2(x, cy - h), ImVec2(x, cy + h), wav, 1.0f);
    }
    dl->AddLine(ImVec2(pos.x, cy), ImVec2(pos.x + w, cy), u32(p.border), 1.0f);
    dl->AddLine(ImVec2(pos.x + w - 1, pos.y), ImVec2(pos.x + w - 1, pos.y + height),
                u32(p.subtle_text), 1.0f);
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
    for (const auto& t : g_pb->topics()) {
        if (t.rfind("/imu/", 0) != 0) continue;
        any = true;
        ImGui::PushFont(fonts::medium(), theme::size::CAPTION);
        ImGui::TextColored(p.subtle_text, "%s", upper(short_topic(t)).c_str());
        ImGui::PopFont();
        imu_plot(t, 120.0f);
        ImGui::Dummy(ImVec2(0, 8));
    }
    if (g_pb->has_audio()) {
        audio_panel(72.0f);
        any = true;
    }
    if (!any) {
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextColored(p.subtle_text, "No /imu/* or /audio topics.");
        ImGui::PopFont();
    }
}

void side_panel(ImVec2 pos, ImVec2 size) {
    const theme::Palette& p = theme::palette();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, pos + size, u32(p.ui));
    dl->AddLine(pos, ImVec2(pos.x, pos.y + size.y), u32(p.border), 1.0f);

    // Header.
    const float head_h = 34.0f;
    dl->AddLine(ImVec2(pos.x, pos.y + head_h), ImVec2(pos.x + size.x, pos.y + head_h),
                u32(p.border), 1.0f);
    ImGui::PushFont(fonts::medium(), theme::size::SMALL);
    dl->AddText(ImVec2(pos.x + 14, pos.y + (head_h - ImGui::GetTextLineHeight()) * 0.5f),
                u32(p.light), "INSPECTOR");
    ImGui::PopFont();

    ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + head_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12, 10));
    ImGui::BeginChild("##sidebody", ImVec2(size.x, size.y - head_h), ImGuiChildFlags_None);

    if (bb::collapsing("TOPICS")) {
        topic_list_body();
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (bb::collapsing("MESSAGE")) {
        inspector_body();
        ImGui::Dummy(ImVec2(0, 4));
    }
    if (bb::collapsing("SENSORS")) {
        sensors_body();
        ImGui::Dummy(ImVec2(0, 4));
    }

    ImGui::EndChild();
    ImGui::PopStyleVar();
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
    g_view.clear();
    g_focus_topic.clear();
    g_rotation = settings::get().default_rotation; // panels seed from this
    if (g_pb->open(utf8_path)) {
        std::fprintf(stderr, "mcap: opened %s (%zu topics, %zu video)\n", utf8_path,
                     g_pb->topics().size(), g_pb->video_topics().size());
        g_pb->set_speed(settings::get().default_speed);
        if (settings::get().autoplay_on_open) g_pb->play();
    } else {
        std::fprintf(stderr, "mcap: failed to open %s\n", utf8_path);
    }
}

bool has_file() { return g_pb && g_pb->is_open(); }
mp::Playback& playback() { return *g_pb; }

// A vertical drag handle on the right panel's left edge. Submitted last (its
// own child window) so it wins input over the video/panel children beneath.
void panel_splitter(ImVec2 panel_pos, float panel_h) {
    const theme::Palette& p = theme::palette();
    const float grab = 10.0f;
    ImGui::SetCursorScreenPos(ImVec2(panel_pos.x - grab * 0.5f, panel_pos.y));
    ImGui::BeginChild("##panelsplit", ImVec2(grab, panel_h), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::InvisibleButton("h", ImVec2(grab, panel_h));
    bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    if (hov || act) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    if (act) {
        // Dragging the handle left (mouse dx < 0) widens the right panel.
        g_panel_w = std::clamp(g_panel_w - ImGui::GetIO().MouseDelta.x, PANEL_W_MIN, PANEL_W_MAX);
    }
    ImGui::GetWindowDrawList()->AddLine(ImVec2(panel_pos.x, panel_pos.y),
                                       ImVec2(panel_pos.x, panel_pos.y + panel_h),
                                       u32(hov || act ? p.accent : p.border),
                                       hov || act ? 2.0f : 1.0f);
    ImGui::EndChild();
}

void layout(ImVec2 o, ImVec2 sz) {
    if (sz.x <= 0 || sz.y <= 0) return;

    // Loop: when playback runs off the end, jump back to the start.
    if (settings::get().loop_at_end && has_file() && g_pb->playing()) {
        uint64_t e = g_pb->end_time_us(), s = g_pb->start_time_us();
        if (e > s && g_pb->current_time_us() + 40'000 >= e) g_pb->seek(s);
    }

    float body_w = sz.x - RAIL_W;
    float panel_w = std::clamp(g_panel_w, PANEL_W_MIN,
                               std::max(PANEL_W_MIN, body_w - 200.0f));
    float region_h = sz.y - TRANSPORT_H;

    ImVec2 rail_pos = o;
    ImVec2 rail_sz(RAIL_W, sz.y);

    ImVec2 transport_pos(o.x + RAIL_W, o.y + region_h);
    ImVec2 transport_sz(body_w, TRANSPORT_H);

    ImVec2 panel_pos(o.x + sz.x - panel_w, o.y);
    ImVec2 panel_sz(panel_w, region_h);

    ImVec2 disp_pos(o.x + RAIL_W, o.y);
    ImVec2 disp_sz(std::max(120.0f, body_w - panel_w), region_h);

    display(disp_pos, disp_sz);
    side_panel(panel_pos, panel_sz);
    transport(transport_pos, transport_sz);
    rail(rail_pos, rail_sz);
    panel_splitter(panel_pos, panel_sz.y);
}

} // namespace mcap_ui
