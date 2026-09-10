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
    std::string display_name;   // short label for the panel header
    int width = 0, height = 0;
    std::string codec;          // "h264" / "h265" / "av1" / ""
    double fps = 0.0;           // nominal frame rate
    double bitrate_bps = 0.0;   // average bitrate
    uint64_t frame_count = 0;
};

// Metadata for a scalar channel (IMU accel/gyro, a LeRobot state/action vector).
struct ScalarChannelInfo {
    bool valid = false;
    std::string display_name;
    int dims = 0;
    std::vector<std::string> dim_labels; // per component (x/y/z, joint names, …)
};

// One timestamped scalar/vector reading. `v.size() == dims`.
struct ScalarSample {
    uint64_t t_us = 0;
    std::vector<float> v;
};

// A playable unit within a recording. MCAP / rosbag = one segment (the whole
// file); a LeRobot dataset = one segment per episode.
struct SegmentInfo {
    std::string name;
    std::string task;         // natural-language task label, if the format has one
    uint64_t duration_us = 0;
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
    // How far into the current segment video frames are actually available.
    // Defaults to "all of it"; a format that streams its frames in the
    // background reports its progress so the clock can wait for the loader.
    virtual uint64_t loaded_until_us() const { return end_time_us(); }
    virtual const std::vector<std::string>& video_channels() const = 0;
    virtual const std::vector<std::string>& scalar_channels() const = 0;
    virtual bool has_audio() const = 0;
    virtual VideoChannelInfo video_info(const std::string& ch) const = 0;
    virtual ScalarChannelInfo scalar_info(const std::string& ch) const = 0;

    // Playable segments. count() is >= 1; MCAP returns 1. select() switches the
    // active segment — after it, start/end time, channels and history reflect
    // the new one. Returns false for an out-of-range index.
    virtual int segment_count() const { return 1; }
    virtual SegmentInfo segment_info(int i) const { (void)i; return {}; }
    virtual bool select_segment(int i) { return i == 0; }
    virtual int current_segment() const { return 0; }

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
