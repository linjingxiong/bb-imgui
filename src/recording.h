// Format-agnostic recording interface. The playback engine (Playback) drives a
// clock and asks a Recording for the frames / samples to show at a given time;
// each format (MCAP, LeRobot, …) implements Recording and owns its own demux +
// decode + seek. This header depends on nothing from Playback or the UI.
#pragma once

#include "video_frame.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace mp {

struct VideoChannelInfo {
    bool valid = false;
    int width = 0, height = 0;
    std::string codec;          // "h264" / "h265" / ""
    double fps = 0.0;           // nominal frame rate
    double bitrate_bps = 0.0;   // average bitrate
    uint64_t frame_count = 0;
};

// One timestamped scalar/vector reading. IMU accel/gyro use dims 3 (xyz);
// a LeRobot state/action vector uses dims up to kMaxDims.
struct ScalarSample {
    static constexpr int kMaxDims = 6;
    uint64_t t_us = 0;
    int dims = 0;
    float v[kMaxDims] = {};
};

// One downsampled mono audio amplitude in [-1, 1] at a log time.
struct AudioPoint {
    uint64_t t_us = 0;
    float amp = 0.0f;
};

class Recording {
public:
    virtual ~Recording() = default;

    // Immutable after a successful open.
    virtual uint64_t start_time_us() const = 0;
    virtual uint64_t end_time_us() const = 0;
    virtual const std::vector<std::string>& video_channels() const = 0;
    virtual const std::vector<std::string>& scalar_channels() const = 0;
    virtual bool has_audio() const = 0;
    virtual VideoChannelInfo video_info(const std::string& ch) const = 0;

    // Whole-recording history, preloaded during open (both formats can do this
    // cheaply — these are small). Returned by const ref; empty for an unknown
    // channel.
    virtual const std::vector<ScalarSample>& scalar_history(const std::string& ch) const = 0;
    virtual const std::vector<AudioPoint>& audio_history() const = 0;

    // Bring every video channel's internal decode cursor to `target_us`. For
    // each channel, write into `out`: a frame (the one to show — the last frame
    // at or before the target), or nullptr to mean "blank this channel" (the
    // target is before its first decodable frame). A channel left absent from
    // `out` keeps whatever it was showing. Channels decode in parallel; the
    // implementation decides per channel whether to decode forward from its
    // current position or flush and replay from a keyframe.
    //
    // Returns false without writing frames if `cancelled()` goes true before
    // the read completes (a newer seek superseded this one) — the decode cursor
    // may still have advanced, which the next call accounts for.
    virtual bool seek_video(uint64_t target_us, const std::function<bool()>& cancelled,
                            std::map<std::string, VideoFramePtr>& out) = 0;
};

// Sniff `path` (file extension / directory layout) and build the matching
// Recording, opened and ready. Returns nullptr if the format is unrecognised
// or opening fails.
std::unique_ptr<Recording> open_recording(const std::string& path);

} // namespace mp
