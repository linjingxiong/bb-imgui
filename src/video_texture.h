// Owns one WGPU texture and keeps it up to date with the latest decoded
// video frame. YUV->RGBA conversion is done CPU-side via libswscale (v1);
// a WGPU YUV sampling shader can replace it later if the copy cost matters.
#pragma once

#include "imgui.h"
#include "video_frame.h"

#include <webgpu/webgpu.h>

namespace mp {

class VideoTexture {
public:
    VideoTexture(WGPUDevice device, WGPUQueue queue);
    ~VideoTexture();

    // Convert `frame` to RGBA and (re)upload. Cheap no-op if frame is null.
    void update(const VideoFramePtr& frame);

    ImTextureID id() const { return (ImTextureID)(intptr_t)view_; }
    int width() const { return w_; }
    int height() const { return h_; }
    bool valid() const { return view_ != nullptr; }

private:
    void ensure_texture(int w, int h);
    void release_texture();

    WGPUDevice device_;
    WGPUQueue queue_;
    WGPUTexture tex_ = nullptr;
    WGPUTextureView view_ = nullptr;
    int w_ = 0, h_ = 0;
    void* sws_ = nullptr; // SwsContext*
    int sws_src_fmt_ = -1;
    std::vector<uint8_t> rgba_;
};

} // namespace mp
