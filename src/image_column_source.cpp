#include "image_column_source.h"

#include <algorithm>
#include <cstdio>

namespace mp {

static std::string qcol(const std::string& n) { return "\"" + n + "\""; }

bool ImageColumnSource::open(const std::string& parquet_path, const std::string& column,
                             int64_t from_index, int64_t to_index) {
    if (!db_.ok()) return false;
    path_ = parquet_path;
    col_ = column;
    from_i_ = from_index;
    to_i_ = to_index;

    ParquetDB::Table t;
    char sql[512];
    std::snprintf(sql, sizeof(sql),
                  "SELECT timestamp FROM read_parquet('%s') "
                  "WHERE \"index\" >= %lld AND \"index\" < %lld ORDER BY \"index\"",
                  sql_path(path_).c_str(), (long long)from_i_, (long long)to_i_);
    if (!db_.query(sql, t) || t.rows == 0) return false;
    const auto* tc = t.col("timestamp");
    ts_.assign(t.rows, 0.0);
    for (size_t r = 0; r < t.rows; ++r) ts_[r] = tc ? tc->num[r] : (double)r / 30.0;
    win_len_us_ = (uint64_t)(ts_.back() * 1e6);

    if (auto f = decode_row(0)) { w_ = f->width; h_ = f->height; }
    ok_ = w_ > 0;
    return ok_;
}

VideoFramePtr ImageColumnSource::decode_row(int local_row) {
    if (local_row < 0 || local_row >= (int)ts_.size()) return nullptr;
    ParquetDB::Table t;
    char sql[512];
    std::snprintf(sql, sizeof(sql),
                  "SELECT %s.bytes AS b FROM read_parquet('%s') WHERE \"index\" = %lld",
                  qcol(col_).c_str(), sql_path(path_).c_str(),
                  (long long)(from_i_ + local_row));
    if (!db_.query(sql, t) || t.rows == 0) return nullptr;
    const auto* bc = t.col("b");
    if (!bc || bc->blob.empty() || bc->blob[0].empty()) return nullptr;
    const auto& raw = bc->blob[0];

    // FFmpeg decoders over-read the packet by up to AV_INPUT_BUFFER_PADDING_SIZE
    // bytes — a std::vector's storage has no such slack, so copy with padding.
    std::vector<uint8_t> bytes(raw.size() + 64, 0);
    std::copy(raw.begin(), raw.end(), bytes.begin());

    const char* codec = "png";
    if (raw.size() > 3 && raw[0] == 0xFF && raw[1] == 0xD8) codec = "jpeg";
    // FFmpeg's PNG decoder keeps a previous-frame reference (for APNG) that
    // avcodec_flush_buffers doesn't clear — the 2nd+ frame decodes corrupt.
    // Tear the decoder down so every still starts from scratch.
    dec_.reset();
    auto f = dec_.decode(codec, bytes.data(), (int)raw.size());
    if (f) f->timestamp_us = (uint64_t)(ts_[local_row] * 1e6);
    return f;
}

VideoFramePtr ImageColumnSource::frame_at(uint64_t target_us,
                                          const std::function<bool()>& cancelled) {
    if (!ok_ || ts_.empty()) return nullptr;
    if (cancelled()) return nullptr;
    double t = target_us / 1e6;
    int row = 0;
    for (int i = 0; i < (int)ts_.size(); ++i) {
        if (ts_[i] <= t) row = i;
        else break;
    }
    if (row == local_cached_ && cached_) return cached_;
    if (cancelled()) return nullptr;
    auto f = decode_row(row);
    if (f) {
        local_cached_ = row;
        cached_ = f;
    }
    return f;
}

} // namespace mp
