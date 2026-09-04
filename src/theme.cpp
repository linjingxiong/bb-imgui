#include "theme.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
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

// Built-in Blockbench "Default (Dark)". Kept for APP_THEME=<bbtheme file> use;
// no longer the default (see ELEMENT_LIGHT below).
const Palette BLOCKBENCH_DARK = {
    /*ui*/ rgb(0x28, 0x2c, 0x34),
    /*back*/ rgb(0x21, 0x25, 0x2b),
    /*deep*/ rgb(0x17, 0x19, 0x1d),
    /*border*/ rgb(0x18, 0x1a, 0x1f),
    /*selected*/ rgb(0x47, 0x4d, 0x5d),
    /*button*/ rgb(0x3a, 0x3f, 0x4b),
    /*bright_ui*/ rgb(0xf4, 0xf3, 0xff),
    /*bright_ui_text*/ rgb(0x00, 0x00, 0x06),
    /*accent*/ rgb(0x3e, 0x90, 0xff),
    /*frame*/ rgb(0x18, 0x1a, 0x1f),
    /*text*/ rgb(0xca, 0xca, 0xd4),
    /*light*/ rgb(0xf4, 0xf3, 0xff),
    /*accent_text*/ rgb(0x00, 0x00, 0x06),
    /*subtle_text*/ rgb(0x84, 0x88, 0x91),
    /*grid*/ rgb(0x49, 0x50, 0x61),
    /*wireframe*/ rgb(0x57, 0x6f, 0x82),
    /*checkerboard*/ rgb(0x1c, 0x20, 0x26),
    /*is_dark*/ true,
};

// Element UI's default light theme. Same Palette field *slots* as the
// Blockbench palette, remapped to Element's colour roles:
//   ui=card bg, back=page bg, deep=input bg, selected=primary-light-9 (hover
//   fill), button=default-button bg, bright_ui/text=dropdown surface,
//   light=strong/emphasis text (darkest, not "near white" as in the dark
//   theme), accent_text=text-on-primary.
const Palette ELEMENT_LIGHT = {
    /*ui*/ rgb(0xff, 0xff, 0xff),
    /*back*/ rgb(0xf2, 0xf3, 0xf5),
    /*deep*/ rgb(0xff, 0xff, 0xff),
    /*border*/ rgb(0xdc, 0xdf, 0xe6),
    /*selected*/ rgb(0xec, 0xf5, 0xff),
    /*button*/ rgb(0xff, 0xff, 0xff),
    /*bright_ui*/ rgb(0xff, 0xff, 0xff),
    /*bright_ui_text*/ rgb(0x30, 0x31, 0x33),
    /*accent*/ rgb(0x40, 0x9e, 0xff),
    /*frame*/ rgb(0xff, 0xff, 0xff),
    /*text*/ rgb(0x60, 0x62, 0x66),
    /*light*/ rgb(0x30, 0x31, 0x33),
    /*accent_text*/ rgb(0xff, 0xff, 0xff),
    /*subtle_text*/ rgb(0x90, 0x93, 0x99),
    /*grid*/ rgb(0xdc, 0xdf, 0xe6),
    /*wireframe*/ rgb(0xc0, 0xc4, 0xcc),
    /*checkerboard*/ rgb(0xf5, 0xf7, 0xfa),
    /*is_dark*/ false,
};

Palette g_current = ELEMENT_LIGHT;

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

Palette load() {
    std::string path;
    if (const char* env = std::getenv("APP_THEME"); env && *env)
        path = env;
    // No APP_THEME -> Element UI's light theme (this branch's default).

    if (path.empty()) return ELEMENT_LIGHT;
    std::string src = read_file(path);
    Palette out;
    if (!src.empty() && parse(src, ELEMENT_LIGHT, out)) return out;
    return ELEMENT_LIGHT;
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

    // Dark theme: nudge panel bg toward white. Light theme: nudge toward the
    // accent colour (Element's hover fill is a light primary tint, not grey).
    const ImVec4 hover_tint = p.is_dark ? p.light : p.accent;
    const ImVec4 hover_bg = mix(p.ui, hover_tint, p.is_dark ? 0.06f : 0.08f);

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
    col[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.45f);
}

const Palette& palette() { return g_current; }

} // namespace theme
