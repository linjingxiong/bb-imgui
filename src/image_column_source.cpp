#include "image_column_source.h"

#include <algorithm>
#include <cstdio>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace mp {

static std::string qcol(const std::string& n) { return "\"" + n + "\""; }

ImageColumnSource::~ImageColumnSource() { stop_loader(); }

void ImageColumnSource::stop_loader() {
    {
        std::lock_guard<std::mutex> lk(ahead_mx_);
        stop_ = true;
    }
    loader_db_.interrupt(); // unblock a chunk query mid-flight
    ahead_cv_.notify_all();
    if (loader_.joinable()) loader_.join();
}

bool ImageColumnSource::open(const std::string& parquet_path, const std::string& column,
                             int episode_index, int w_hint, int h_hint) {
    return start(parquet_path, column, episode_index, w_hint, h_hint);
}

bool ImageColumnSource::reopen(const std::string& parquet_path, const std::string& column,
                               int episode_index, int w_hint, int h_hint) {
    stop_loader();
    {
        std::lock_guard<std::mutex> lk(ahead_mx_);
        stop_ = false;
        ahead_want_ = -1;
    }
    {
        std::lock_guard<std::mutex> lk(lru_mx_);
        lru_.clear();
    }
    ts_.clear();
    blobs_.clear();
    bulk_upto_.store(0, std::memory_order_release);
    ok_ = false;
    return start(parquet_path, column, episode_index, w_hint, h_hint);
}

bool ImageColumnSource::start(const std::string& parquet_path, const std::string& column,
                              int episode_index, int w_hint, int h_hint) {
    if (!db_.ok()) return false;
    path_ = parquet_path;
    col_ = column;
    ep_ = episode_index;
    w_ = w_hint > 0 ? w_hint : 0;
    h_ = h_hint > 0 ? h_hint : 0;

    ParquetDB::Table t;
    char sql[512];
    std::snprintf(sql, sizeof(sql),
                  "SELECT timestamp FROM read_parquet('%s') "
                  "WHERE \"episode_index\" = %d ORDER BY \"frame_index\"",
                  sql_path(path_).c_str(), ep_);
    if (!db_.query(sql, t) || t.rows == 0) return false;
    const auto* tc = t.col("timestamp");
    ts_.assign(t.rows, 0.0);
    for (size_t r = 0; r < t.rows; ++r) ts_[r] = tc ? tc->num[r] : (double)r / 30.0;
    win_len_us_ = (uint64_t)(ts_.back() * 1e6);

    blobs_.resize(ts_.size());
    ok_ = true;
    loader_ = std::thread(&ImageColumnSource::loader_main, this);
    return true;
}

uint64_t ImageColumnSource::loaded_until_us() const {
    int n = bulk_upto_.load(std::memory_order_acquire);
    if (n <= 0 || ts_.empty()) return 0;
    if (n >= (int)ts_.size()) return UINT64_MAX; // fully loaded — never the limit
    return (uint64_t)(ts_[n - 1] * 1e6);
}

int ImageColumnSource::row_for(uint64_t target_us) const {
    const double t = target_us / 1e6;
    int row = 0;
    for (int i = 0; i < (int)ts_.size(); ++i) {
        if (ts_[i] <= t) row = i;
        else break;
    }
    return row;
}

VideoFramePtr ImageColumnSource::decode(const std::vector<uint8_t>& raw, int local_row,
                                        VideoDecoder& dec) {
    if (raw.empty()) return nullptr;
    // FFmpeg decoders over-read the packet by up to AV_INPUT_BUFFER_PADDING_SIZE
    // bytes — a std::vector's storage has no such slack, so copy with padding.
    std::vector<uint8_t> bytes(raw.size() + 64, 0);
    std::copy(raw.begin(), raw.end(), bytes.begin());

    const char* codec = "png";
    if (raw.size() > 3 && raw[0] == 0xFF && raw[1] == 0xD8) codec = "jpeg";
    // FFmpeg's PNG decoder keeps a previous-frame reference (for APNG) that
    // avcodec_flush_buffers doesn't clear — the 2nd+ frame decodes corrupt.
    // Tear the decoder down so every still starts from scratch.
    dec.reset();
    auto f = dec.decode(codec, bytes.data(), (int)raw.size());
    if (f && local_row >= 0 && local_row < (int)ts_.size())
        f->timestamp_us = (uint64_t)(ts_[local_row] * 1e6);
    return f;
}

VideoFramePtr ImageColumnSource::lru_get(int local_row) {
    std::lock_guard<std::mutex> lk(lru_mx_);
    for (auto it = lru_.begin(); it != lru_.end(); ++it) {
        if (it->first == local_row) {
            if (it != lru_.begin()) lru_.splice(lru_.begin(), lru_, it);
            return lru_.front().second;
        }
    }
    return nullptr;
}

void ImageColumnSource::lru_put(int local_row, const VideoFramePtr& f) {
    if (!f) return;
    std::lock_guard<std::mutex> lk(lru_mx_);
    for (auto it = lru_.begin(); it != lru_.end(); ++it)
        if (it->first == local_row) { lru_.erase(it); break; }
    lru_.emplace_front(local_row, f);
    while (lru_.size() > kLruCap) lru_.pop_back();
}

VideoFramePtr ImageColumnSource::get_frame(int local_row, VideoDecoder& dec) {
    if (auto hit = lru_get(local_row)) return hit;
    if (local_row < 0 || local_row >= (int)blobs_.size()) return nullptr;
    if (local_row >= bulk_upto_.load(std::memory_order_acquire)) return nullptr;
    auto f = decode(blobs_[local_row], local_row, dec);
    if (f && w_ == 0) { w_ = f->width; h_ = f->height; }
    lru_put(local_row, f);
    return f;
}

VideoFramePtr ImageColumnSource::frame_at(uint64_t target_us,
                                          const std::function<bool()>& cancelled) {
    if (!ok_ || ts_.empty() || cancelled()) return nullptr;
    int row = row_for(target_us);

    // Clamp to what the loader has delivered — playback drifts a touch behind
    // for the first moment rather than blocking on a query. Before the loader
    // has produced anything, hold on frame 0 (seeded in start()).
    const int have = bulk_upto_.load(std::memory_order_acquire);
    if (have == 0) row = 0;
    else if (row >= have) row = have - 1;

    auto f = get_frame(row, dec_);

    { // nudge the look-ahead
        std::lock_guard<std::mutex> lk(ahead_mx_);
        ahead_want_ = row + 1;
    }
    ahead_cv_.notify_one();
    return f;
}

void ImageColumnSource::loader_main() {
#if defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    const int n = (int)ts_.size();

    // Phase 1: pull the episode's image bytes in row chunks — frame 0 alone
    // first (cheap: a single row's worth of decode), then steady 32-row
    // batches. bulk_upto_ climbs after each chunk, so a channel is "ready"
    // (frame_at can serve row 0) within the first chunk rather than after the
    // whole episode loads. The leading SELECT absorbs any stale interrupt flag
    // left by the previous stop.
    {
        ParquetDB::Table tmp;
        loader_db_.query("SET threads TO 1", tmp);
        // Continue from whatever start() already seeded (usually frame 0).
        int start = bulk_upto_.load(std::memory_order_acquire), chunk = 1;
        while (start < n) {
            {
                std::lock_guard<std::mutex> lk(ahead_mx_);
                if (stop_) return;
            }
            const int end = std::min(start + chunk, n);
            char sql[640];
            std::snprintf(sql, sizeof(sql),
                          "SELECT %s.bytes AS b FROM read_parquet('%s') "
                          "WHERE \"episode_index\" = %d AND \"frame_index\" >= %d "
                          "AND \"frame_index\" < %d ORDER BY \"frame_index\"",
                          qcol(col_).c_str(), sql_path(path_).c_str(), ep_, start, end);
            ParquetDB::Table t;
            if (!loader_db_.query(sql, t) || t.rows == 0) return; // error or interrupted
            const auto* bc = t.col("b");
            if (!bc) return;
            for (size_t r = 0; r < t.rows && start + (int)r < n; ++r)
                blobs_[start + r] = std::move(bc->blob[r]);
            bulk_upto_.store(end, std::memory_order_release);
            start = end;
            // Modest, steady chunks keep bulk_upto_ climbing smoothly so the
            // playhead isn't left stalled behind a big read.
            chunk = 32;
        }
    }

    // Phase 2: forward look-ahead — decode a small window past the last frame
    // the UI asked for, into the shared LRU.
    for (;;) {
        int want = -1;
        {
            std::unique_lock<std::mutex> lk(ahead_mx_);
            ahead_cv_.wait(lk, [&] { return stop_ || ahead_want_ >= 0; });
            if (stop_) return;
            want = ahead_want_;
            ahead_want_ = -1;
        }
        for (int r = want; r < want + kLookahead && r < n; ++r) {
            {
                std::lock_guard<std::mutex> lk(ahead_mx_);
                if (stop_ || ahead_want_ >= 0) break; // superseded
            }
            if (r >= bulk_upto_.load(std::memory_order_acquire)) break;
            if (lru_get(r)) continue;
            lru_put(r, decode(blobs_[r], r, ahead_dec_));
        }
    }
}

} // namespace mp
