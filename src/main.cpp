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

#include <cstdio>
#include <cstdlib>

#include <GLFW/glfw3.h>
#include <webgpu/webgpu.h>

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

    g_surface_config.device = g_device;
    g_surface_config.format = caps.formats[0];
    g_surface_config.usage = WGPUTextureUsage_RenderAttachment;
    g_surface_config.presentMode = WGPUPresentMode_Fifo;
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
int main(int, char**) {
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
    io.IniFilename = nullptr; // TODO(phase 2): persist layout

    ImGui::StyleColorsDark();

    ImGui_ImplGlfw_InitForOther(window, true);
    ImGui_ImplWGPU_InitInfo init_info;
    init_info.Device = g_device;
    init_info.NumFramesInFlight = 3;
    init_info.RenderTargetFormat = g_surface_config.format;
    init_info.DepthStencilFormat = WGPUTextureFormat_Undefined;
    ImGui_ImplWGPU_Init(&init_info);

    const ImVec4 clear = ImVec4(0.157f, 0.173f, 0.204f, 1.0f); // Blockbench "ui"

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (glfwGetWindowAttrib(window, GLFW_ICONIFIED)) {
            ImGui_ImplGlfw_Sleep(10);
            continue;
        }

        int w, h;
        glfwGetFramebufferSize(window, &w, &h);
        if (w != g_surface_w || h != g_surface_h)
            resize_surface(w, h);

        WGPUSurfaceTexture surface_texture;
        wgpuSurfaceGetCurrentTexture(g_surface, &surface_texture);
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

        const ImGuiID dockspace_id =
            ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());

        static bool layout_done = false;
        if (!layout_done) {
            layout_done = true;
            ImGui::DockBuilderRemoveNode(dockspace_id);
            ImGui::DockBuilderAddNode(dockspace_id, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspace_id,
                                          ImGui::GetMainViewport()->WorkSize);
            ImGuiID center = dockspace_id;
            ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.22f,
                                                       nullptr, &center);
            ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.24f,
                                                        nullptr, &center);
            ImGui::DockBuilderDockWindow("Left", left);
            ImGui::DockBuilderDockWindow("Right", right);
            ImGui::DockBuilderDockWindow("Workspace", center);
            ImGui::DockBuilderFinish(dockspace_id);
        }

        ImGui::Begin("Left");
        ImGui::TextUnformatted("nav / outliner (phase 2)");
        ImGui::End();

        ImGui::Begin("Workspace");
        ImGui::TextUnformatted("central workspace");
        ImGui::Separator();
        ImGui::Text("%.1f FPS", (double)io.Framerate);
        ImGui::End();

        ImGui::Begin("Right");
        ImGui::TextUnformatted("inspector (phase 2)");
        ImGui::End();

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
        wgpuQueueSubmit(g_queue, 1, &cmd);
        wgpuSurfacePresent(g_surface);

        wgpuCommandBufferRelease(cmd);
        wgpuRenderPassEncoderRelease(pass);
        wgpuCommandEncoderRelease(encoder);
        wgpuTextureViewRelease(view);
        wgpuTextureRelease(surface_texture.texture);
    }

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
