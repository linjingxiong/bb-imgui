// A "video" channel whose frames are PNG/JPEG bytes stored in a parquet
// column (LeRobot `dtype: "image"` — a struct<bytes, path> per row).
// Timestamps are episode-relative (from the `timestamp` column).
//
// open() only runs one light query (episode timestamps); every image byte is
// pulled in the background in row chunks (parse-once, no per-frame parquet
// scan), and frames are decoded on demand from memory with a small LRU and a
// forward look-ahead. Rows not yet loaded clamp to the newest loaded row so
// playback drifts a little behind at first rather than stalling.
#pragma once

#include "parquet.h"
#include "video_decoder.h"
#include "video_frame.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp {

class ImageColumnSource {
public:
    ImageColumnSource() = default;
    ~ImageColumnSource();

    // `parquet_path` is the file holding this episode's rows; `column` the
    // image feature key; `episode_index` selects the rows (via the parquet's
    // episode_index column); w_hint/h_hint are the dimensions from
    // meta/info.json (0 if unknown — then filled from the first decoded frame).
    bool open(const std::string& parquet_path, const std::string& column, int episode_index,
              int w_hint, int h_hint);
    // Point an already-open source at a different episode, reusing its DuckDB
    // connection and decoders (cheap — no open/close churn on an episode switch).
    bool reopen(const std::string& parquet_path, const std::string& column, int episode_index,
                int w_hint, int h_hint);
    bool is_open() const { return ok_; }
    int width() const { return w_; }
    int height() const { return h_; }
    uint64_t window_len_us() const { return win_len_us_; }
    // Episode-relative time up to which frames have been bulk-loaded (0 until
    // the loader produces the first, win_len_us_ once it finishes).
    uint64_t loaded_until_us() const;

    // Last frame at or before `target_us` (window-relative), or null.
    VideoFramePtr frame_at(uint64_t target_us, const std::function<bool()>& cancelled);

private:
    bool start(const std::string& parquet_path, const std::string& column, int episode_index,
               int w_hint, int h_hint);
    void stop_loader();
    int row_for(uint64_t target_us) const;
    VideoFramePtr decode(const std::vector<uint8_t>& raw, int local_row, VideoDecoder& dec);
    VideoFramePtr get_frame(int local_row, VideoDecoder& dec);
    VideoFramePtr lru_get(int local_row);
    void lru_put(int local_row, const VideoFramePtr& f);
    void loader_main();

    ParquetDB db_;        // open()/start() ts query — UI thread, never interrupted
    ParquetDB loader_db_; // the loader thread's connection — interrupted on stop
    std::string path_, col_;
    int ep_ = 0;
    std::vector<double> ts_; // episode-relative seconds, index == local row (== frame_index)
    uint64_t win_len_us_ = 0;

    VideoDecoder dec_;       // frame_at thread
    VideoDecoder ahead_dec_; // loader thread

    std::vector<std::vector<uint8_t>> blobs_; // sized in open(); loader fills, publishes via bulk_upto_
    std::atomic<int> bulk_upto_{0};

    std::mutex lru_mx_;
    std::list<std::pair<int, VideoFramePtr>> lru_; // front == most recently used
    static constexpr size_t kLruCap = 24;
    static constexpr int kLookahead = 3;

    std::thread loader_;
    std::mutex ahead_mx_;
    std::condition_variable ahead_cv_;
    int ahead_want_ = -1;
    bool stop_ = false;

    int w_ = 0, h_ = 0;
    bool ok_ = false;
};

} // namespace mp
