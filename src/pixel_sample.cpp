#include "pixel_sample.h"

#include <algorithm>
#include <cmath>

namespace mp {
namespace {

unsigned char clamp8(double v) {
    return (unsigned char)std::clamp((int)std::lround(v), 0, 255);
}

// YUV -> RGB for one pixel, matching the coefficients video_texture.cpp hands
// swscale (BT.601 vs BT.709, limited vs full/JPEG range).
void yuv_to_rgb(int Y, int U, int V, bool full_range, bool bt601, unsigned char rgb[3]) {
    const double u = U - 128.0, v = V - 128.0;
    double y = Y;
    double kr, kb;
    if (bt601) { kr = 0.299; kb = 0.114; }
    else       { kr = 0.2126; kb = 0.0722; }
    const double kg = 1.0 - kr - kb;

    if (!full_range) {
        y = (Y - 16.0) * (255.0 / 219.0);
        const double uv = 255.0 / 224.0;
        const double r = y + (2.0 - 2.0 * kr) * uv * v;
        const double b = y + (2.0 - 2.0 * kb) * uv * u;
        const double g = y - (2.0 - 2.0 * kr) * kr / kg * uv * v -
                         (2.0 - 2.0 * kb) * kb / kg * uv * u;
        rgb[0] = clamp8(r);
        rgb[1] = clamp8(g);
        rgb[2] = clamp8(b);
        return;
    }
    const double r = y + (2.0 - 2.0 * kr) * v;
    const double b = y + (2.0 - 2.0 * kb) * u;
    const double g = y - (2.0 - 2.0 * kr) * kr / kg * v - (2.0 - 2.0 * kb) * kb / kg * u;
    rgb[0] = clamp8(r);
    rgb[1] = clamp8(g);
    rgb[2] = clamp8(b);
}

int stride_of(const VideoFrame& f, int plane, int fallback) {
    return f.strides[plane] > 0 ? f.strides[plane] : fallback;
}

} // namespace

bool sample_rgb(const VideoFrame& f, int x, int y, unsigned char rgb[3]) {
    if (x < 0 || y < 0 || x >= f.width || y >= f.height) return false;

    if (f.format == VideoFrame::PixelFormat::Rgb24) {
        const int s = stride_of(f, 0, f.width * 3);
        const size_t off = (size_t)y * s + (size_t)x * 3;
        if (off + 2 >= f.planes[0].size()) return false;
        rgb[0] = f.planes[0][off + 0];
        rgb[1] = f.planes[0][off + 1];
        rgb[2] = f.planes[0][off + 2];
        return true;
    }

    if (f.format == VideoFrame::PixelFormat::Yuv420P) {
        const int ys = stride_of(f, 0, f.width);
        const int us = stride_of(f, 1, f.width / 2);
        const int vs = stride_of(f, 2, f.width / 2);
        const size_t yo = (size_t)y * ys + x;
        const size_t co = (size_t)(y / 2) * us + (x / 2);
        const size_t cv = (size_t)(y / 2) * vs + (x / 2);
        if (yo >= f.planes[0].size() || co >= f.planes[1].size() || cv >= f.planes[2].size())
            return false;
        yuv_to_rgb(f.planes[0][yo], f.planes[1][co], f.planes[2][cv], f.full_range, f.bt601, rgb);
        return true;
    }

    if (f.format == VideoFrame::PixelFormat::Nv12) {
        const int ys = stride_of(f, 0, f.width);
        const int cs = stride_of(f, 1, f.width);
        const size_t yo = (size_t)y * ys + x;
        const size_t co = (size_t)(y / 2) * cs + (x / 2) * 2;
        if (yo >= f.planes[0].size() || co + 1 >= f.planes[1].size()) return false;
        yuv_to_rgb(f.planes[0][yo], f.planes[1][co], f.planes[1][co + 1], f.full_range, f.bt601, rgb);
        return true;
    }

    return false;
}

} // namespace mp
