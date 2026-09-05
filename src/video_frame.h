// A decoded video frame's pixel data — Qt-free port of EgoViewer's
// domain/VideoFrameBuffer.h. The decoder hands these out; video_texture.cpp
// converts them to RGBA and uploads to a WGPU texture.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace mp {

struct VideoFrame {
    enum class PixelFormat { Yuv420P, Nv12 };

    PixelFormat format = PixelFormat::Yuv420P;
    int width = 0;
    int height = 0;
    // Y / U / V (Yuv420P) or Y / interleaved-UV (Nv12); planes[2] unused for Nv12.
    std::vector<uint8_t> planes[3];
    int strides[3] = {0, 0, 0};
    uint64_t timestamp_us = 0;
    bool full_range = false; // AVCOL_RANGE_JPEG
    bool bt601 = false;      // else BT.709
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;

} // namespace mp
