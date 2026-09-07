#include "theme.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace theme {
namespace {

constexpr ImVec4 rgb(int r, int g, int b) {
    return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, 1.0f);
}

// Built-in Blockbench "Default (Dark)" — pulled verbatim from the real
// themes/dark.bbtheme in JannisX11/blockbench (some of these had drifted
// from an earlier, eyeballed pass; accent/text/light/accent_text/
// subtle_text/wireframe already matched, the rest didn't).
const Palette BLOCKBENCH_DARK = {
    /*ui*/ rgb(0x1e, 0x21, 0x27),
    /*back*/ rgb(0x18, 0x1b, 0x1f),
    /*deep*/ rgb(0x10, 0x13, 0x16),   // --color-dark
    /*border*/ rgb(0x10, 0x13, 0x16), // --color-border
    /*selected*/ rgb(0x3b, 0x3e, 0x49),
    /*button*/ rgb(0x33, 0x38, 0x3f),
    /*bright_ui*/ rgb(0xf4, 0xf3, 0xff),
    /*bright_ui_text*/ rgb(0x00, 0x00, 0x06),
    /*accent*/ rgb(0x3e, 0x90, 0xff),
    /*frame*/ rgb(0x0f, 0x10, 0x12),
    /*text*/ rgb(0xca, 0xca, 0xd4),
    /*light*/ rgb(0xf4, 0xf3, 0xff),
    /*accent_text*/ rgb(0x00, 0x00, 0x06),
    /*subtle_text*/ rgb(0x84, 0x88, 0x91),
    /*grid*/ rgb(0x30, 0x33, 0x3d),
    /*wireframe*/ rgb(0x57, 0x6f, 0x82),
    /*checkerboard*/ rgb(0x14, 0x17, 0x1b),
    /*is_dark*/ true,
};

// Ohwow (ohwow.design) — a near-monochrome dark theme. Colours sampled from
// a screenshot of the app; there's no coloured accent (it uses brightness,
// not hue, for emphasis), so `accent` is a light grey and selected states
// read as light-on-dark.
const Palette OHWOW_DARK = {
    /*ui*/ rgb(0x22, 0x1f, 0x23),
    /*back*/ rgb(0x13, 0x13, 0x13),
    /*deep*/ rgb(0x02, 0x02, 0x02),
    /*border*/ rgb(0x11, 0x11, 0x11),
    /*selected*/ rgb(0x37, 0x37, 0x37),
    /*button*/ rgb(0x37, 0x37, 0x37),
    /*bright_ui*/ rgb(0x1f, 0x1d, 0x20),
    /*bright_ui_text*/ rgb(0xc9, 0xc7, 0xca),
    /*accent*/ rgb(0xe6, 0xe6, 0xe6),
    /*frame*/ rgb(0x13, 0x13, 0x13),
    /*text*/ rgb(0xc9, 0xc7, 0xca),
    /*light*/ rgb(0xf2, 0xf2, 0xf2),
    /*accent_text*/ rgb(0x0a, 0x0a, 0x0a),
    /*subtle_text*/ rgb(0x70, 0x70, 0x70),
    /*grid*/ rgb(0x2a, 0x2a, 0x2a),
    /*wireframe*/ rgb(0x50, 0x50, 0x50),
    /*checkerboard*/ rgb(0x14, 0x14, 0x14),
    /*is_dark*/ true,
};

Palette g_current = BLOCKBENCH_DARK;

int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

float linearize(float s) {
    return s <= 0.04045f ? s / 12.92f : std::pow((s + 0.055f) / 1.055f, 2.4f);
}

float luminance(const ImVec4& c) {
    return 0.2126f * linearize(c.x) + 0.7152f * linearize(c.y) + 0.0722f * linearize(c.z);
}

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

ImVec4 mix(const ImVec4& a, const ImVec4& b, float t) {
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t,
                  a.w + (b.w - a.w) * t);
}

ImVec4 with_alpha(const ImVec4& c, float a) { return ImVec4(c.x, c.y, c.z, a); }

// Fill a palette from a bbtheme JSON object, falling back to `base` per key.
bool parse(const std::string& src, const Palette& base, Palette& out) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(src);
    } catch (...) {
        return false;
    }
    const auto& c = j.value("colors", nlohmann::json::object());
    auto pick = [&](const char* key, const ImVec4& fallback) -> ImVec4 {
        if (!c.contains(key) || !c[key].is_string()) return fallback;
        ImVec4 v;
        return parse_hex(c[key].get<std::string>(), v) ? v : fallback;
    };

    out = base;
    out.ui = pick("ui", base.ui);
    out.back = pick("back", base.back);
    out.deep = pick("dark", base.deep);
    out.border = pick("border", base.border);
    out.selected = pick("selected", base.selected);
    out.button = pick("button", base.button);
    out.bright_ui = pick("bright_ui", base.bright_ui);
    out.bright_ui_text = pick("bright_ui_text", base.bright_ui_text);
    out.accent = pick("accent", base.accent);
    out.frame = pick("frame", base.frame);
    out.text = pick("text", base.text);
    out.light = pick("light", base.light);
    out.accent_text = pick("accent_text", base.accent_text);
    out.subtle_text = pick("subtle_text", base.subtle_text);
    out.grid = pick("grid", base.grid);
    out.wireframe = pick("wireframe", base.wireframe);
    out.checkerboard = pick("checkerboard", base.checkerboard);
    out.is_dark = luminance(out.ui) < 0.5f;
    return true;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

bool parse_hex(const std::string& in, ImVec4& out) {
    std::string s = in;
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
    if (!s.empty() && s.front() == '#') s.erase(s.begin());

    auto n1 = [&](size_t i) { return hex_nibble(s[i]); };
    auto n2 = [&](size_t i) { return hex_nibble(s[i]) * 16 + hex_nibble(s[i + 1]); };

    int r, g, b, a = 255;
    if (s.size() == 3 || s.size() == 4) {
        for (char ch : s)
            if (hex_nibble(ch) < 0) return false;
        r = n1(0) * 17;
        g = n1(1) * 17;
        b = n1(2) * 17;
        if (s.size() == 4) a = n1(3) * 17;
    } else if (s.size() == 6 || s.size() == 8) {
        for (char ch : s)
            if (hex_nibble(ch) < 0) return false;
        r = n2(0);
        g = n2(2);
        b = n2(4);
        if (s.size() == 8) a = n2(6);
    } else {
        return false;
    }
    out = ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
    return true;
}

// ── Theme registry ──────────────────────────────────────────────────────
namespace {

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    Palette palette;
    std::string path;                     // empty for a built-in
    fs::file_time_type mtime{};
    bool builtin = false;
};

std::vector<Entry> g_themes;
std::vector<std::string> g_names;         // cached list() result
std::string g_current_name;

std::string themes_dir() { return exe_dir() + "/assets/themes"; }
std::string settings_path() { return exe_dir() + "/theme.txt"; }

// The name from a theme JSON's top-level "name" (falls back to the file stem).
std::string theme_name_from(const std::string& src, const fs::path& p) {
    try {
        auto j = nlohmann::json::parse(src, nullptr, false);
        if (j.is_object() && j.contains("name") && j["name"].is_string())
            return j["name"].get<std::string>();
    } catch (...) {}
    return p.stem().string();
}

void refresh_names() {
    g_names.clear();
    for (const auto& e : g_themes) g_names.push_back(e.name);
}

Entry* find(const std::string& name) {
    for (auto& e : g_themes)
        if (e.name == name) return &e;
    return nullptr;
}

void build_registry() {
    g_themes.clear();
    g_themes.push_back({"Ohwow", OHWOW_DARK, {}, {}, true});
    g_themes.push_back({"Blockbench Dark", BLOCKBENCH_DARK, {}, {}, true});

    std::error_code ec;
    for (const auto& de : fs::directory_iterator(themes_dir(), ec)) {
        if (ec) break;
        if (!de.is_regular_file()) continue;
        auto ext = de.path().extension().string();
        if (ext != ".json" && ext != ".bbtheme") continue;
        std::string src = read_file(de.path().string());
        if (src.empty()) continue;
        Palette pal;
        if (!parse(src, BLOCKBENCH_DARK, pal)) continue;
        std::string name = theme_name_from(src, de.path());
        auto tm = fs::last_write_time(de.path(), ec);
        // A file with a built-in's name overrides it (so editing the file
        // live-tunes that theme); otherwise it's a new entry.
        if (Entry* existing = find(name)) {
            existing->palette = pal;
            existing->path = de.path().string();
            existing->mtime = tm;
            existing->builtin = false;
        } else {
            g_themes.push_back({name, pal, de.path().string(), tm, false});
        }
    }
    refresh_names();
}

} // namespace

const std::vector<std::string>& list() { return g_names; }
const std::string& current() { return g_current_name; }

void set(const std::string& name) {
    Entry* e = find(name);
    if (!e) return;
    g_current_name = name;
    apply(e->palette);
    std::ofstream(settings_path(), std::ios::trunc) << name;
}

void cycle() {
    if (g_names.empty()) return;
    auto it = std::find(g_names.begin(), g_names.end(), g_current_name);
    size_t next = (it == g_names.end()) ? 0 : (size_t)(it - g_names.begin() + 1) % g_names.size();
    set(g_names[next]);
}

void rescan() {
    build_registry();
    // Keep the current selection if it still exists, else fall back.
    if (!find(g_current_name) && !g_names.empty()) set(g_names.front());
}

void poll_hot_reload() {
    Entry* e = find(g_current_name);
    if (!e || e->path.empty()) return;
    std::error_code ec;
    auto tm = fs::last_write_time(e->path, ec);
    if (ec || tm == e->mtime) return;
    std::string src = read_file(e->path);
    Palette pal;
    if (!src.empty() && parse(src, BLOCKBENCH_DARK, pal)) {
        e->palette = pal;
        e->mtime = tm;
        apply(pal);
    }
}

Palette load() {
    build_registry();

    std::string want;
    if (const char* env = std::getenv("APP_THEME"); env && *env) {
        // Back-compat: APP_THEME as a path — parse it as a one-off.
        std::string src = read_file(env);
        Palette out;
        if (!src.empty() && parse(src, BLOCKBENCH_DARK, out)) {
            apply(out);
            g_current_name = "(APP_THEME)";
            return out;
        }
    }
    if (std::ifstream f{settings_path()}) std::getline(f, want);
    if (want.empty() || !find(want)) want = "Blockbench Dark";
    set(want);
    return g_current;
}

void apply(const Palette& p) {
    g_current = p;

    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = RADIUS;
    s.FrameRounding = RADIUS;
    s.PopupRounding = 6.0f;
    s.ScrollbarRounding = RADIUS;
    s.GrabRounding = RADIUS;
    s.TabRounding = RADIUS;
    s.WindowBorderSize = 1.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.PopupBorderSize = 1.0f;
    s.TabBorderSize = 0.0f;
    s.WindowPadding = ImVec2(8, 8);
    // ImGui frame height = font size + 2*FramePadding.y. BODY ≈ 21 → +9 ≈ 30
    // (Blockbench .bar / .tool height).
    s.FramePadding = ImVec2(8, 4);
    s.ItemSpacing = ImVec2(8, 5);
    s.ItemInnerSpacing = ImVec2(6, 4);
    s.CellPadding = ImVec2(8, 4);
    s.ScrollbarSize = 11.0f;
    s.GrabMinSize = 9.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;
    s.SeparatorTextBorderSize = 1.0f;
    s.DockingSeparatorSize = 2.0f;

    const ImVec4 hover_tint = p.is_dark ? p.light : p.deep;
    const ImVec4 hover_bg = mix(p.ui, hover_tint, 0.06f);

    ImVec4* col = s.Colors;
    col[ImGuiCol_Text] = p.text;
    col[ImGuiCol_TextDisabled] = p.subtle_text;
    col[ImGuiCol_WindowBg] = p.ui;
    col[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_PopupBg] = p.bright_ui;
    col[ImGuiCol_Border] = p.border;
    col[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_FrameBg] = p.deep;
    col[ImGuiCol_FrameBgHovered] = mix(p.deep, p.selected, 0.4f);
    col[ImGuiCol_FrameBgActive] = mix(p.deep, p.selected, 0.6f);
    col[ImGuiCol_TitleBg] = p.back;
    col[ImGuiCol_TitleBgActive] = p.back;
    col[ImGuiCol_TitleBgCollapsed] = p.back;
    col[ImGuiCol_MenuBarBg] = p.back;
    col[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ScrollbarGrab] = p.button;
    col[ImGuiCol_ScrollbarGrabHovered] = p.selected;
    col[ImGuiCol_ScrollbarGrabActive] = p.accent;
    col[ImGuiCol_CheckMark] = p.accent;
    col[ImGuiCol_SliderGrab] = p.accent;
    col[ImGuiCol_SliderGrabActive] = mix(p.accent, p.light, 0.2f);
    col[ImGuiCol_Button] = p.button;
    col[ImGuiCol_ButtonHovered] = with_alpha(p.selected, 0.7f);
    col[ImGuiCol_ButtonActive] = p.selected;
    col[ImGuiCol_Header] = p.selected;
    col[ImGuiCol_HeaderHovered] = with_alpha(p.selected, 0.7f);
    col[ImGuiCol_HeaderActive] = p.selected;
    col[ImGuiCol_Separator] = p.border;
    col[ImGuiCol_SeparatorHovered] = p.selected;
    col[ImGuiCol_SeparatorActive] = p.accent;
    col[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_ResizeGripHovered] = with_alpha(p.selected, 0.6f);
    col[ImGuiCol_ResizeGripActive] = p.accent;
    col[ImGuiCol_Tab] = p.back;
    col[ImGuiCol_TabHovered] = hover_bg;
    col[ImGuiCol_TabSelected] = p.ui;
    col[ImGuiCol_TabSelectedOverline] = p.accent;
    col[ImGuiCol_TabDimmed] = p.back;
    col[ImGuiCol_TabDimmedSelected] = p.ui;
    col[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0, 0, 0, 0);
    col[ImGuiCol_DockingPreview] = with_alpha(p.accent, 0.5f);
    col[ImGuiCol_DockingEmptyBg] = p.deep;
    col[ImGuiCol_PlotLines] = p.subtle_text;
    col[ImGuiCol_PlotLinesHovered] = p.accent;
    col[ImGuiCol_PlotHistogram] = p.accent;
    col[ImGuiCol_PlotHistogramHovered] = mix(p.accent, p.light, 0.2f);
    col[ImGuiCol_TextSelectedBg] = with_alpha(p.accent, p.is_dark ? 0.35f : 0.22f);
    col[ImGuiCol_NavCursor] = p.accent;
    col[ImGuiCol_DragDropTarget] = p.accent;
    col[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f); // Blockbench dialog backdrop
}

const Palette& palette() { return g_current; }

} // namespace theme
