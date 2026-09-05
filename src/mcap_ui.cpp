#include "mcap_ui.h"

#include "bb.h"
#include "fonts.h"
#include "icons.h"
#include "playback.h"
#include "theme.h"
#include "video_texture.h"

#include "imgui.h"

#include <cstdio>
#include <map>
#include <memory>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#include <commdlg.h>
#endif

namespace mcap_ui {
namespace {

WGPUDevice g_device = nullptr;
WGPUQueue g_queue = nullptr;
std::unique_ptr<mp::Playback> g_pb;
std::map<std::string, std::unique_ptr<mp::VideoTexture>> g_textures;

ImU32 u32(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

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
        bool is_video = false;
        for (const auto& vt : g_pb->video_topics())
            if (vt == t) { is_video = true; break; }
        char cnt[24];
        std::snprintf(cnt, sizeof(cnt), "%llu", (unsigned long long)g_pb->message_count(t));
        bb::outliner_node(t.c_str(), /*leaf=*/true, /*selected=*/false, nullptr, t.c_str());
        ImVec2 rmin = ImGui::GetItemRectMin(), rmax = ImGui::GetItemRectMax();
        ImGui::PushFont(nullptr, theme::size::SMALL * 0.9f);
        ImVec2 ts = ImGui::CalcTextSize(cnt);
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rmax.x - ts.x - 24.0f, rmin.y + (rmax.y - rmin.y - ts.y) * 0.5f),
            u32(p.subtle_text), cnt);
        ImGui::PopFont();
        (void)is_video;
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

    for (size_t i = 0; i < vts.size(); ++i) {
        const std::string& topic = vts[i];
        if (i % cols != 0) ImGui::SameLine(0, spacing);

        auto& tex = g_textures[topic];
        if (!tex) tex = std::make_unique<mp::VideoTexture>(g_device, g_queue);
        if (auto frame = g_pb->latest_frame(topic)) tex->update(frame);

        float cell_h = tex->valid() && tex->width() > 0
                           ? cell_w * (float)tex->height() / (float)tex->width()
                           : cell_w * 9.0f / 16.0f;

        ImGui::BeginChild((topic + "##vid").c_str(), ImVec2(cell_w, cell_h + 22.0f),
                          ImGuiChildFlags_Borders);
        ImGui::PushFont(nullptr, theme::size::SMALL);
        ImGui::TextUnformatted(short_topic(topic));
        ImGui::PopFont();
        if (tex->valid())
            ImGui::Image(tex->id(), ImVec2(cell_w - 4.0f, cell_h));
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

    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace mcap_ui
