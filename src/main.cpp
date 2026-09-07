// bb-imgui — a Dear ImGui app styled after Blockbench.
//
// Phase 0: borderless GLFW window + WebGPU (wgpu-native) renderer + docking.
//
// The WebGPU init / surface plumbing is adapted from Dear ImGui's
// examples/example_glfw_wgpu (wgpu-native path).

#include "imgui.h"
#include "imgui_internal.h" // DockBuilder
#include "imgui_impl_glfw.h"
#include "imgui_impl_wgpu.h"

#include "bb.h"
#include "fonts.h"
#include "gallery.h"
#include "icons.h"
#include "logo.h"
#include "mcap_ui.h"
#include "playback.h"
#include "menu.h"
#include "shell.h"
#include "theme.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>
#ifdef BB_PROFILE
#include <chrono>
#endif

#include <GLFW/glfw3.h>
#include <webgpu/webgpu.h>

// ---------------------------------------------------------------------------
// Title-bar menus (mirrors Blockbench's menu bar)
// ---------------------------------------------------------------------------
namespace {
using menu::Item;
const Item SEP{};

const Item FILE_ITEMS[] = {
    {ICON_PHOTO_LIBRARY, "Open MCAP\xe2\x80\xa6"},
    SEP,
    {ICON_FILE, "New", nullptr, false, true},
    {ICON_HISTORY, "Recent", nullptr, false, true},
    {ICON_FOLDER_OPEN, "Open Model\xe2\x80\xa6", "Ctrl+O"},
    {ICON_NEW_WINDOW, "New Window"},
    SEP,
    {ICON_SAVE, "Save", "Ctrl+S"},
    {ICON_SAVE_AS, "Save As\xe2\x80\xa6", "Ctrl+Shift+S"},
    {ICON_CLOSE, "Close Project"},
    SEP,
    {ICON_FILE, "Import", nullptr, false, true},
    {ICON_FILE, "Export", nullptr, false, true},
    SEP,
    {ICON_SETTINGS, "Settings\xe2\x80\xa6"},
    {ICON_KEYBOARD, "Keybindings\xe2\x80\xa6"},
    {ICON_DARK_MODE, "Themes\xe2\x80\xa6"},
};
const Item EDIT_ITEMS[] = {
    {ICON_UNDO, "Undo", "Ctrl+Z"},
    {ICON_REDO, "Redo", "Ctrl+Y"},
    {ICON_HISTORY, "Edit History\xe2\x80\xa6"},
    SEP,
    {ICON_ADD, "Add Cube", "Ctrl+N"},
    {ICON_ADD, "Add Group", "Ctrl+G"},
    SEP,
    {ICON_COPY, "Copy", "Ctrl+C"},
    {ICON_PASTE, "Paste", "Ctrl+V"},
    {ICON_EDIT, "Rename", "F2"},
    {ICON_DELETE, "Delete", "Del"},
    SEP,
    {ICON_SEARCH, "Find / Replace\xe2\x80\xa6"},
    {ICON_VISIBILITY, "Select All", "Ctrl+A"},
};
const Item TRANSFORM_ITEMS[] = {
    {ICON_SETTINGS_OVERSCAN, "Scale\xe2\x80\xa6"},
    {ICON_ROTATE, "Rotate", nullptr, false, true},
    {ICON_FLIP, "Flip", nullptr, false, true},
    {ICON_CENTER_FOCUS, "Center", nullptr, false, true},
};
const Item VIEW_ITEMS[] = {
    {ICON_FULLSCREEN, "Fullscreen", "F11"},
    {ICON_GRID, "Toggle Grid", nullptr, true},
    {ICON_VISIBILITY, "Toggle Wireframe", "Z"},
    SEP,
    {ICON_CENTER_FOCUS, "Camera Angle", nullptr, false, true},
    SEP,
    {ICON_TUNE, "Component gallery\xe2\x80\xa6"},
};
const Item TOOLS_ITEMS[] = {
    {ICON_BRUSH, "Paint Brush", "B"},
    {ICON_TUNE, "Color Picker", "I"},
    SEP,
    {ICON_FILE, "Plugins\xe2\x80\xa6"},
};
const Item HELP_ITEMS[] = {
    {ICON_HELP, "Quickstart"},
    {ICON_FILE, "Documentation"},
    SEP,
    {ICON_INFO, "About"},
};

#define MENU(name, arr) menu::Menu{name, arr, (int)(sizeof(arr) / sizeof(arr[0]))}
const menu::Menu MENUS[] = {
    MENU("File", FILE_ITEMS),       MENU("Edit", EDIT_ITEMS),
    MENU("Transform", TRANSFORM_ITEMS), MENU("Tools", TOOLS_ITEMS),
    MENU("View", VIEW_ITEMS),       MENU("Help", HELP_ITEMS),
};

// The View menu, rebuilt each frame so the theme list (and its checkmarks)
// stays live. Returns a menu array to hand to shell::set_menus().
const menu::Menu* build_menus() {
    static std::vector<menu::Item> view;
    view.assign(std::begin(VIEW_ITEMS), std::end(VIEW_ITEMS));
    view.push_back(menu::separator());
    for (const std::string& n : theme::list()) {
        menu::Item it{};
        it.icon = ICON_PALETTE;
        it.label = n.c_str(); // stable within this frame (theme::list() owns it)
        it.checked = (n == theme::current());
        view.push_back(it);
    }
    { menu::Item it{}; it.icon = ICON_HISTORY; it.label = "Reload themes"; view.push_back(it); }

    static menu::Menu menus[] = {
        MENU("File", FILE_ITEMS),       MENU("Edit", EDIT_ITEMS),
        MENU("Transform", TRANSFORM_ITEMS), MENU("Tools", TOOLS_ITEMS),
        {"View", nullptr, 0},           MENU("Help", HELP_ITEMS),
    };
    menus[4].items = view.data();
    menus[4].count = (int)view.size();
    return menus;
}
#undef MENU
} // namespace

// ---------------------------------------------------------------------------
// WebGPU context
// ---------------------------------------------------------------------------
namespace {

WGPUInstance             g_instance = nullptr;
WGPUDevice               g_device = nullptr;
WGPUSurface              g_surface = nullptr;
WGPUQueue                g_queue = nullptr;
WGPUSurfaceConfiguration g_surface_config = {};
int                      g_surface_w = 1440;
int                      g_surface_h = 880;

void glfw_error_callback(int error, const char* description) {
    std::fprintf(stderr, "GLFW error %d: %s\n", error, description);
}

void resize_surface(int w, int h) {
    if (w <= 0 || h <= 0)
        return; // wgpu rejects a zero-area surface (minimised window)
    g_surface_config.width = g_surface_w = w;
    g_surface_config.height = g_surface_h = h;
    wgpuSurfaceConfigure(g_surface, &g_surface_config);
}

void handle_request_adapter(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                            WGPUStringView message, void* userdata1, void*) {
    if (status != WGPURequestAdapterStatus_Success) {
        std::fprintf(stderr, "wgpu: request adapter failed: %.*s\n",
                     (int)message.length, message.data);
        return;
    }
    *(WGPUAdapter*)userdata1 = adapter;
}

void handle_request_device(WGPURequestDeviceStatus status, WGPUDevice device,
                           WGPUStringView message, void* userdata1, void*) {
    if (status != WGPURequestDeviceStatus_Success) {
        std::fprintf(stderr, "wgpu: request device failed: %.*s\n",
                     (int)message.length, message.data);
        return;
    }
    *(WGPUDevice*)userdata1 = device;
}

WGPUAdapter request_adapter(WGPUInstance instance) {
    WGPURequestAdapterOptions opts = {};
    WGPUAdapter adapter = nullptr;
    WGPURequestAdapterCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_WaitAnyOnly;
    cb.callback = handle_request_adapter;
    cb.userdata1 = &adapter;
    wgpuInstanceRequestAdapter(instance, &opts, cb);
    IM_ASSERT(adapter && "wgpu: no adapter");
    return adapter;
}

WGPUDevice request_device(WGPUAdapter adapter) {
    WGPUDevice device = nullptr;
    WGPURequestDeviceCallbackInfo cb = {};
    cb.mode = WGPUCallbackMode_WaitAnyOnly;
    cb.callback = handle_request_device;
    cb.userdata1 = &device;
    wgpuAdapterRequestDevice(adapter, nullptr, cb);
    IM_ASSERT(device && "wgpu: no device");
    return device;
}

} // namespace

// GLFW native surface helper (from example_glfw_wgpu).
#ifdef _WIN32
#undef APIENTRY
#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#else
#define GLFW_EXPOSE_NATIVE_X11
#if defined(__has_include) && __has_include(<wayland-client.h>)
#define GLFW_EXPOSE_NATIVE_WAYLAND
#endif
#endif
#include <GLFW/glfw3native.h>
#undef Status

static WGPUSurface create_surface(WGPUInstance instance, GLFWwindow* window) {
    ImGui_ImplWGPU_CreateSurfaceInfo info = {};
    info.Instance = instance;
#if defined(GLFW_EXPOSE_NATIVE_COCOA)
    info.System = "cocoa";
    info.RawWindow = (void*)glfwGetCocoaWindow(window);
#elif defined(GLFW_EXPOSE_NATIVE_WAYLAND)
    if (glfwGetPlatform() == GLFW_PLATFORM_WAYLAND) {
        info.System = "wayland";
        info.RawDisplay = (void*)glfwGetWaylandDisplay();
        info.RawSurface = (void*)glfwGetWaylandWindow(window);
        return ImGui_ImplWGPU_CreateWGPUSurfaceHelper(&info);
    }
    info.System = "x11";
    info.RawWindow = (void*)glfwGetX11Window(window);
    info.RawDisplay = (void*)glfwGetX11Display();
#elif defined(GLFW_EXPOSE_NATIVE_X11)
    info.System = "x11";
    info.RawWindow = (void*)glfwGetX11Window(window);
    info.RawDisplay = (void*)glfwGetX11Display();
#elif defined(GLFW_EXPOSE_NATIVE_WIN32)
    info.System = "win32";
    info.RawWindow = (void*)glfwGetWin32Window(window);
    info.RawInstance = (void*)::GetModuleHandle(nullptr);
#endif
    return ImGui_ImplWGPU_CreateWGPUSurfaceHelper(&info);
}

static bool init_wgpu(GLFWwindow* window) {
    WGPUInstanceDescriptor desc = {};
    WGPUInstanceFeatureName timed_wait = WGPUInstanceFeatureName_TimedWaitAny;
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = &timed_wait;
    g_instance = wgpuCreateInstance(&desc);

    wgpuSetLogCallback(
        [](WGPULogLevel level, WGPUStringView msg, void*) {
            std::fprintf(stderr, "wgpu [%s]: %.*s\n",
                         ImGui_ImplWGPU_GetLogLevelName(level), (int)msg.length, msg.data);
        },
        nullptr);
    wgpuSetLogLevel(WGPULogLevel_Warn);

    WGPUAdapter adapter = request_adapter(g_instance);
    ImGui_ImplWGPU_DebugPrintAdapterInfo(adapter);
    g_device = request_device(adapter);

    g_surface = create_surface(g_instance, window);
    if (!g_surface)
        return false;

    WGPUSurfaceCapabilities caps = {};
    wgpuSurfaceGetCapabilities(g_surface, adapter, &caps);

    // ImGui works in gamma space — prefer a non-sRGB surface format so colours
    // are presented as authored.
    WGPUTextureFormat fmt = caps.formats[0];
    for (size_t i = 0; i < caps.formatCount; i++) {
        if (caps.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
            caps.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
            fmt = caps.formats[i];
            break;
        }
    }

    // Present mode: default to Fifo (vsync, always presents a frame). Mailbox
    // is lower-latency on a normal desktop, but on this dev machine's ToDesk
    // remote-desktop session Mailbox silently presents nothing at all (the
    // window stays a valid, responding, on-screen window per the OS, but no
    // frame ever reaches the screen) — so don't auto-select it. Override with
    // WGPU_PRESENT=fifo|mailbox|immediate.
    WGPUPresentMode present = WGPUPresentMode_Fifo;
    if (const char* e = std::getenv("WGPU_PRESENT")) {
        if (std::strcmp(e, "fifo") == 0) present = WGPUPresentMode_Fifo;
        else if (std::strcmp(e, "mailbox") == 0) present = WGPUPresentMode_Mailbox;
        else if (std::strcmp(e, "immediate") == 0) present = WGPUPresentMode_Immediate;
    }
    std::fprintf(stderr, "wgpu: %zu present modes; using %d\n", caps.presentModeCount,
                 (int)present);

    g_surface_config.device = g_device;
    g_surface_config.format = fmt;
    g_surface_config.usage = WGPUTextureUsage_RenderAttachment;
    g_surface_config.presentMode = present;
    g_surface_config.alphaMode = WGPUCompositeAlphaMode_Auto;
    g_surface_config.width = g_surface_w;
    g_surface_config.height = g_surface_h;
    wgpuSurfaceConfigure(g_surface, &g_surface_config);

    g_queue = wgpuDeviceGetQueue(g_device);
    return true;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
        return 1;

    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API); // WebGPU manages the context
    glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);   // we draw our own title bar (phase 2)

    GLFWwindow* window = glfwCreateWindow(g_surface_w, g_surface_h, "Blockbench", nullptr, nullptr);
    if (!window)
        return 1;

    if (!init_wgpu(window)) {
        std::fprintf(stderr, "wgpu init failed\n");
        return 1;
    }
    glfwShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigWindowsMoveFromTitleBarOnly = true;
    io.IniFilename = nullptr; // TODO: persist layout under a real path

    theme::load(); // builds the theme registry + applies the remembered theme
    fonts::install(1.0f);

    static const shell::ProjectTab tabs[] = {
        {"MCAP Player", false},
    };
    shell::set_tabs(tabs, (int)(sizeof(tabs) / sizeof(tabs[0])));
    bool gallery_open = false; // dev: View > Component gallery still toggles the widget browser

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplWGPU_InitInfo init_info;
    init_info.Device = g_device;
    init_info.NumFramesInFlight = 3;
    init_info.RenderTargetFormat = g_surface_config.format;
    init_info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    ImGui_ImplWGPU_Init(&init_info);

    logo::load(g_device, g_queue, 19.0f); // Blockbench wordmark for the title bar
    mcap_ui::init(g_device, g_queue);
    if (argc > 1) mcap_ui::open_path(argv[1]); // CLI: bb_imgui <file.mcap> [play]
    if (argc > 2 && std::strcmp(argv[2], "play") == 0 && mcap_ui::has_file())
        mcap_ui::playback().play();

    const ImVec4 clear = ImVec4(0.157f, 0.173f, 0.204f, 1.0f); // Blockbench "ui"

    // Mailbox present doesn't block on vsync, so cap the loop to the monitor's
    // refresh rate (a bit of headroom) rather than spinning a core at 1000s fps.
    int refresh = 60;
    if (GLFWmonitor* mon = glfwGetPrimaryMonitor())
        if (const GLFWvidmode* vm = glfwGetVideoMode(mon))
            refresh = vm->refreshRate > 0 ? vm->refreshRate : 60;
    const double frame_budget = 1.0 / (refresh + 10);
    double next_frame = glfwGetTime();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED) || w <= 0 || h <= 0) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }
        if (w != g_surface_w || h != g_surface_h)
            resize_surface(w, h);

#ifdef BB_PROFILE
        auto _t0 = std::chrono::high_resolution_clock::now();
#endif
        WGPUSurfaceTexture surface_texture;
        wgpuSurfaceGetCurrentTexture(g_surface, &surface_texture);
#ifdef BB_PROFILE
        auto _t1 = std::chrono::high_resolution_clock::now();
#endif
        if (ImGui_ImplWGPU_IsSurfaceStatusError(surface_texture.status)) {
            std::fprintf(stderr, "wgpu: surface status %#.8x\n", surface_texture.status);
            std::abort();
        }
        if (ImGui_ImplWGPU_IsSurfaceStatusSubOptimal(surface_texture.status)) {
            if (surface_texture.texture)
                wgpuTextureRelease(surface_texture.texture);
            if (w > 0 && h > 0)
                resize_surface(w, h);
            continue;
        }

        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        char status_r[64];
        std::snprintf(status_r, sizeof(status_r), "%.0f FPS", (double)io.Framerate);
        // Left side mirrors Blockbench's status bar: project breadcrumb +
        // the same modifier-key hints shown in the reference screenshot.
        const char* active_tab = shell::tabs_active() >= 0 ? tabs[shell::tabs_active()].name : "";
        char status_l[160];
        std::snprintf(status_l, sizeof(status_l),
                     "%s      Ctrl  Select multiple      Shift  Select range      "
                     "Alt  Drag to duplicate",
                     active_tab);
        shell::set_status(status_l, status_r);
        shell::set_status_tab("COLLECTIONS");

        theme::poll_hot_reload();
        shell::set_menus(build_menus(), 6);
        // The MCAP player runs in Minimal chrome (slim title bar only); the
        // dev-only component gallery still wants Blockbench's Full chrome.
        shell::set_chrome(gallery_open ? shell::Chrome::Full : shell::Chrome::Minimal);
        shell::begin(window);

        if (const char* a = shell::menu_clicked()) {
            if (std::strstr(a, "Component gallery"))
                gallery_open = !gallery_open;
            else if (std::strstr(a, "Open MCAP") || std::strstr(a, "Open Model"))
                mcap_ui::open_dialog();
            else if (std::strcmp(a, "Reload themes") == 0)
                theme::rescan();
            else {
                for (const std::string& n : theme::list())
                    if (n == a) { theme::set(n); break; }
            }
        }

        if (gallery_open) {
            // Toolbar row: panel name + a few representative Blockbench tool
            // icons + the Edit/Paint/Animate mode selector (right-aligned).
            shell::toolbar_begin(shell::mode() == shell::Mode::Edit ? "UV"
                                 : shell::mode() == shell::Mode::Paint ? "Paint"
                                                                       : "Animate");
            static int active_tool = 0;
            const char* tools[] = {ICON_MOVE, ICON_ROTATE, ICON_RESIZE, ICON_PIVOT, ICON_BRUSH};
            for (int i = 0; i < 5; i++)
                if (shell::tool_button(tools[i], active_tool == i)) active_tool = i;
            shell::toolbar_end();

            if (bb::begin_panel("Left", false)) gallery::list();
            bb::end_panel();
            if (bb::begin_panel("Workspace", false)) gallery::detail();
            bb::end_panel();
            if (bb::begin_panel("Right", false)) {}
            bb::end_panel();
        } else {
            ImVec2 area_pos, area_size;
            shell::content_rect(&area_pos, &area_size);
            mcap_ui::layout(area_pos, area_size);
        }

        shell::end();

        ImGui::Render();

        WGPUTextureViewDescriptor view_desc = {};
        view_desc.format = g_surface_config.format;
        view_desc.dimension = WGPUTextureViewDimension_2D;
        view_desc.mipLevelCount = WGPU_MIP_LEVEL_COUNT_UNDEFINED;
        view_desc.arrayLayerCount = WGPU_ARRAY_LAYER_COUNT_UNDEFINED;
        view_desc.aspect = WGPUTextureAspect_All;
        WGPUTextureView view = wgpuTextureCreateView(surface_texture.texture, &view_desc);

        WGPURenderPassColorAttachment color = {};
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = WGPULoadOp_Clear;
        color.storeOp = WGPUStoreOp_Store;
        color.clearValue = {clear.x * clear.w, clear.y * clear.w, clear.z * clear.w, clear.w};
        color.view = view;

        WGPURenderPassDescriptor pass_desc = {};
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;

        WGPUCommandEncoderDescriptor enc_desc = {};
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(g_device, &enc_desc);
        WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_desc);
        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), pass);
        wgpuRenderPassEncoderEnd(pass);

        WGPUCommandBufferDescriptor cmd_desc = {};
        WGPUCommandBuffer cmd = wgpuCommandEncoderFinish(encoder, &cmd_desc);
#ifdef BB_PROFILE
        auto _t2 = std::chrono::high_resolution_clock::now();
#endif
        wgpuQueueSubmit(g_queue, 1, &cmd);
        wgpuSurfacePresent(g_surface);
        wgpuInstanceProcessEvents(g_instance);
#ifdef BB_PROFILE
        auto _t3 = std::chrono::high_resolution_clock::now();
        static double _acc = 0; static int _n = 0;
        auto _ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        _acc += _ms(_t0, _t3);
        if (++_n >= 30) {
            std::fprintf(stderr,
                         "frame %.1fms | acquire %.1f build %.1f submit %.1f | draws %d verts %d "
                         "texupdates %d\n",
                         _acc / _n, _ms(_t0, _t1), _ms(_t1, _t2), _ms(_t2, _t3),
                         ImGui::GetDrawData()->CmdListsCount, ImGui::GetDrawData()->TotalVtxCount,
                         ImGui::GetDrawData()->Textures ? ImGui::GetDrawData()->Textures->Size : -1);
            _acc = 0; _n = 0;
        }
#endif

        wgpuCommandBufferRelease(cmd);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);

        next_frame += frame_budget;
        double now = glfwGetTime();
        if (next_frame > now)
            ImGui_ImplGlfw_Sleep((int)((next_frame - now) * 1000.0));
        else
            next_frame = now; // fell behind — don't accumulate debt
    }

    mcap_ui::shutdown();
    ImGui_ImplWGPU_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    wgpuSurfaceUnconfigure(g_surface);
    wgpuSurfaceRelease(g_surface);
    wgpuQueueRelease(g_queue);
    wgpuDeviceRelease(g_device);
    wgpuInstanceRelease(g_instance);
    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}
