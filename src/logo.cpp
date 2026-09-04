#include "logo.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "third_party/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvgrast.h"

#include <webgpu/webgpu.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace logo {
namespace {

WGPUTextureView g_view = nullptr;
int g_w = 0, g_h = 0;

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
    auto s = p.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : p.substr(0, s);
}

} // namespace

bool load(WGPUDevice device, WGPUQueue queue, float target_h) {
    const std::string path = exe_dir() + "/assets/blockbench-logo.svg";
    NSVGimage* img = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!img || img->width <= 0 || img->height <= 0) {
        if (img) nsvgDelete(img);
        return false;
    }

    const float px_h = target_h * 2.0f; // supersample
    const float scale = px_h / img->height;
    const int w = (int)std::ceil(img->width * scale);
    const int h = (int)std::ceil(px_h);
    nsvgDelete(img);

    // Re-parse (rasterizer needs the image again after we read its size).
    img = nsvgParseFromFile(path.c_str(), "px", 96.0f);
    if (!img) return false;

    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, img, 0.0f, 0.0f, scale, rgba.data(), w, h, w * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(img);

    WGPUTextureDescriptor td = {};
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    WGPUTexture tex = wgpuDeviceCreateTexture(device, &td);
    if (!tex) return false;

    WGPUTexelCopyTextureInfo dst = {};
    dst.texture = tex;
    dst.mipLevel = 0;
    dst.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = {};
    layout.offset = 0;
    layout.bytesPerRow = (uint32_t)w * 4;
    layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext = {(uint32_t)w, (uint32_t)h, 1};
    wgpuQueueWriteTexture(queue, &dst, rgba.data(), rgba.size(), &layout, &ext);

    WGPUTextureViewDescriptor vd = {};
    vd.format = WGPUTextureFormat_RGBA8Unorm;
    vd.dimension = WGPUTextureViewDimension_2D;
    vd.baseMipLevel = 0;
    vd.mipLevelCount = 1;
    vd.baseArrayLayer = 0;
    vd.arrayLayerCount = 1;
    vd.aspect = WGPUTextureAspect_All;
    g_view = wgpuTextureCreateView(tex, &vd);

    g_w = w;
    g_h = h;
    return g_view != nullptr;
}

ImTextureID texture() { return (ImTextureID)(intptr_t)g_view; }
ImVec2 pixel_size() { return ImVec2((float)g_w, (float)g_h); }

} // namespace logo
