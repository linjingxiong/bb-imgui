// A "video" channel whose frames are PNG/JPEG bytes stored in a parquet
// column (LeRobot `dtype: "image"` — a struct<bytes, path> per row). Frames
// are decoded on demand; timestamps are episode-relative (from the
// `timestamp` column).
#pragma once

#include "parquet.h"
#include "video_decoder.h"
#include "video_frame.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace mp {

class ImageColumnSource {
public:
    // `parquet_path` is the episode's data file; `column` the image feature
    // key; [from_index, to_index) the episode's global row range.
    bool open(const std::string& parquet_path, const std::string& column, int64_t from_index,
              int64_t to_index);
    bool is_open() const { return ok_; }
    int width() const { return w_; }
    int height() const { return h_; }
    uint64_t window_len_us() const { return win_len_us_; }

    // Last frame at or before `target_us` (window-relative), or null.
    VideoFramePtr frame_at(uint64_t target_us, const std::function<bool()>& cancelled);

private:
    VideoFramePtr decode_row(int local_row);

    ParquetDB db_;
    std::string path_, col_;
    int64_t from_i_ = 0, to_i_ = 0;
    std::vector<double> ts_; // episode-relative seconds, index == local row
    uint64_t win_len_us_ = 0;
    VideoDecoder dec_;
    int local_cached_ = -1;
    VideoFramePtr cached_;
    int w_ = 0, h_ = 0;
    bool ok_ = false;
};

} // namespace mp
