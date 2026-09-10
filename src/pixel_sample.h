// Read the RGB colour of a single source pixel out of a decoded VideoFrame,
// converting YUV planes on the fly. Used by the pixel inspector.
#pragma once

#include "video_frame.h"

namespace mp {

// Fill rgb[3] with source pixel (x, y). Returns false if (x, y) is out of
// range or the frame's format isn't understood.
bool sample_rgb(const VideoFrame& f, int x, int y, unsigned char rgb[3]);

} // namespace mp
