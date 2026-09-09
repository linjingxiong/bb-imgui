#include "video_texture.h"

#include <cstring>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace mp {

VideoTexture::VideoTexture(WGPUDevice device, WGPUQueue queue)
    : device_(device), queue_(queue) {}

VideoTexture::~VideoTexture() {
    release_texture();
    if (sws_) sws_freeContext(static_cast<SwsContext*>(sws_));
}

void VideoTexture::release_texture() {
    if (view_) { wgpuTextureViewRelease(view_); view_ = nullptr; }
    if (tex_) { wgpuTextureRelease(tex_); tex_ = nullptr; }
}

void VideoTexture::ensure_texture(int w, int h) {
    if (tex_ && w == w_ && h == h_) return;
    release_texture();
    w_ = w;
    h_ = h;

    WGPUTextureDescriptor td = {};
    td.usage = WGPUTextureUsage_TextureBinding | WGPUTextureUsage_CopyDst;
    td.dimension = WGPUTextureDimension_2D;
    td.size = {(uint32_t)w, (uint32_t)h, 1};
    td.format = WGPUTextureFormat_RGBA8Unorm;
    td.mipLevelCount = 1;
    td.sampleCount = 1;
    tex_ = wgpuDeviceCreateTexture(device_, &td);
    if (!tex_) return;

    WGPUTextureViewDescriptor vd = {};
    vd.format = WGPUTextureFormat_RGBA8Unorm;
    vd.dimension = WGPUTextureViewDimension_2D;
    vd.mipLevelCount = 1;
    vd.arrayLayerCount = 1;
    vd.aspect = WGPUTextureAspect_All;
    view_ = wgpuTextureCreateView(tex_, &vd);
}

void VideoTexture::update(const VideoFramePtr& frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return;
    const int w = frame->width, h = frame->height;
    ensure_texture(w, h);
    if (!tex_) return;

    rgba_.resize((size_t)w * h * 4);

    // Packed RGB (PNG/JPEG frames) — a straight expand to RGBA, no swscale.
    if (frame->format == VideoFrame::PixelFormat::Rgb24) {
        const int ss = frame->strides[0] > 0 ? frame->strides[0] : w * 3;
        const uint8_t* s = frame->planes[0].data();
        uint8_t* d = rgba_.data();
        for (int y = 0; y < h; ++y) {
            const uint8_t* sr = s + (size_t)y * ss;
            uint8_t* dr = d + (size_t)y * w * 4;
            for (int x = 0; x < w; ++x) {
                dr[x * 4 + 0] = sr[x * 3 + 0];
                dr[x * 4 + 1] = sr[x * 3 + 1];
                dr[x * 4 + 2] = sr[x * 3 + 2];
                dr[x * 4 + 3] = 255;
            }
        }
        WGPUTexelCopyTextureInfo di = {};
        di.texture = tex_;
        di.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyBufferLayout la = {};
        la.bytesPerRow = (uint32_t)w * 4;
        la.rowsPerImage = (uint32_t)h;
        WGPUExtent3D ex = {(uint32_t)w, (uint32_t)h, 1};
        wgpuQueueWriteTexture(queue_, &di, rgba_.data(), rgba_.size(), &la, &ex);
        return;
    }

    const int src_fmt = frame->format == VideoFrame::PixelFormat::Nv12
                            ? AV_PIX_FMT_NV12
                            : (frame->full_range ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P);

    auto* sws = static_cast<SwsContext*>(sws_);
    if (!sws || src_fmt != sws_src_fmt_) {
        if (sws) sws_freeContext(sws);
        sws = sws_getContext(w, h, (AVPixelFormat)src_fmt, w, h, AV_PIX_FMT_RGBA,
                             SWS_BILINEAR, nullptr, nullptr, nullptr);
        sws_ = sws;
        sws_src_fmt_ = src_fmt;
        if (sws) {
            const int table = frame->bt601 ? SWS_CS_ITU601 : SWS_CS_ITU709;
            sws_setColorspaceDetails(sws, sws_getCoefficients(table), frame->full_range,
                                     sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                                     1 << 16, 1 << 16);
        }
    }
    if (!sws) return;

    const uint8_t* src[4] = {frame->planes[0].data(), frame->planes[1].data(),
                             frame->planes[2].data(), nullptr};
    int src_stride[4] = {frame->strides[0], frame->strides[1], frame->strides[2], 0};

    uint8_t* dst[4] = {rgba_.data(), nullptr, nullptr, nullptr};
    int dst_stride[4] = {w * 4, 0, 0, 0};
    sws_scale(sws, src, src_stride, 0, h, dst, dst_stride);

    WGPUTexelCopyTextureInfo dstinfo = {};
    dstinfo.texture = tex_;
    dstinfo.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferLayout layout = {};
    layout.bytesPerRow = (uint32_t)w * 4;
    layout.rowsPerImage = (uint32_t)h;
    WGPUExtent3D ext = {(uint32_t)w, (uint32_t)h, 1};
    wgpuQueueWriteTexture(queue_, &dstinfo, rgba_.data(), rgba_.size(), &layout, &ext);
}

} // namespace mp
