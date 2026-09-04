#include "fonts.h"

#include "icons.h"
#include "theme.h"

#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace fonts {
namespace {

ImFont* g_body = nullptr;
ImFont* g_medium = nullptr;

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

// Merge the icon font onto whatever font was added just before.
void merge_icons(const std::string& icons_path) {
    static const ImWchar range[] = {ICON_MIN, ICON_MAX, 0};
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.PixelSnapH = true;
    // Setting glyph advances requires a non-zero reference size (dynamic font
    // system still scales the merged source with its parent).
    cfg.GlyphMinAdvanceX = theme::size::BODY;
    ImGui::GetIO().Fonts->AddFontFromFileTTF(icons_path.c_str(), theme::size::BODY, &cfg,
                                             range);
}

} // namespace

void install(float /*dpi_scale*/) {
    ImGuiIO& io = ImGui::GetIO();
    const std::string dir = exe_dir() + "/assets/fonts/";
    const std::string regular = dir + "Assistant-Regular.ttf";
    const std::string semibold = dir + "Assistant-SemiBold.ttf";
    const std::string icons = dir + "material-icons.ttf";

    ImGui::GetStyle().FontSizeBase = theme::size::BODY;

    g_body = io.Fonts->AddFontFromFileTTF(regular.c_str(), theme::size::BODY);
    if (g_body)
        merge_icons(icons);
    else
        g_body = io.Fonts->AddFontDefault();

    g_medium = io.Fonts->AddFontFromFileTTF(semibold.c_str(), theme::size::BODY);
    if (g_medium)
        merge_icons(icons);
    else
        g_medium = g_body;

    io.FontDefault = g_body;
}

ImFont* body() { return g_body; }
ImFont* medium() { return g_medium; }

} // namespace fonts
