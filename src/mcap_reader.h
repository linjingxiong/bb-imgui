// Qt-free port of EgoViewer's src/data/McapReader.{h,cpp}. Reads MCAP
// recordings written by EgoViewer's own McapWriter — understands only the
// schemas it writes (foxglove.CompressedVideo, foxglove.RawAudio,
// ego.ImuSample, JSON /rgb_controls). Not a general Foxglove viewer.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mp {

struct McapMessage {
    std::string topic;
    uint64_t timestamp_us = 0; // from mcap's nanosecond logTime
    std::vector<uint8_t> data;
    std::string schema_name;
    std::string message_encoding;
};

class McapReader {
public:
    McapReader();
    ~McapReader();

    bool open(const std::string& path);
    void close();
    bool is_open() const;

    uint64_t start_time_us() const;
    uint64_t end_time_us() const;

    // Nearest keyframe-safe chunk start at or before target_us, with ~1s of
    // decoder warm-up lead-in (see the EgoViewer comment). Falls back to
    // start_time_us().
    uint64_t seekable_start_time_us(uint64_t target_us) const;

    // Topics that carry at least one message in this file.
    std::set<std::string> topics_with_messages() const;

    // Total message count per topic, from the summary statistics (0 if the
    // file has no statistics section).
    std::map<std::string, uint64_t> message_totals() const;

    // The embedded "ego_metadata" Metadata record's JSON, or "" if absent.
    std::string metadata_json(const std::string& name = "ego_metadata") const;

    // Iterate messages with logTime in [start_us, end_us) in increasing time
    // order. end_us == 0 means "to end of file". Return false from cb to stop.
    using MessageCallback = std::function<bool(const McapMessage&)>;
    bool read_messages(uint64_t start_us, uint64_t end_us, const MessageCallback& cb);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// ── Decoders for EgoViewer's own MCAP schemas ──

struct DecodedCompressedVideo {
    uint64_t timestamp_us = 0;
    std::string frame_id;
    std::vector<uint8_t> data;
    std::string format; // "h264" / "h265"
};
bool decode_compressed_video(const std::vector<uint8_t>& payload, DecodedCompressedVideo& out);

struct DecodedRawAudio {
    uint64_t timestamp_us = 0;
    std::vector<uint8_t> data;
    std::string format;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
};
bool decode_raw_audio(const std::vector<uint8_t>& payload, DecodedRawAudio& out);

struct DecodedImuSample {
    uint64_t timestamp_us = 0;
    double x = 0, y = 0, z = 0;
};
bool decode_imu_sample(const std::vector<uint8_t>& payload, DecodedImuSample& out);

} // namespace mp
