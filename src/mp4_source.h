// Decodes one video stream of an mp4, restricted to a time window
// [from_us, to_us) — a LeRobot episode's slice of a shared file. Frame
// timestamps are reported relative to from_us (window starts at 0). Uses
// libavformat's demux + index (no manual keyframe scanning).
#pragma once

#include "video_frame.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mp {

class Mp4Source {
public:
    Mp4Source();
    ~Mp4Source();
    Mp4Source(const Mp4Source&) = delete;
    Mp4Source& operator=(const Mp4Source&) = delete;

    // Open `path`, pick the first video stream, restrict playback to
    // [from_us, to_us). to_us == 0 means "to end of file".
    bool open(const std::string& path, uint64_t from_us, uint64_t to_us);
    bool is_open() const { return ff_ != nullptr; }

    int width() const { return width_; }
    int height() const { return height_; }
    const std::string& codec() const { return codec_name_; }
    uint64_t window_len_us() const { return win_len_us_; }

    // Bring the decode cursor to `target_us` (window-relative) and return the
    // last frame at or before it, or null before the first frame / on failure.
    // Decodes forward when the target is just ahead of the cursor, else seeks
    // to the preceding keyframe. `cancelled()` true aborts (returns null).
    VideoFramePtr frame_at(uint64_t target_us, const std::function<bool()>& cancelled);

private:
    void close();

    struct FF;
    std::unique_ptr<FF> ff_;
    int width_ = 0, height_ = 0;
    std::string codec_name_;
    uint64_t from_us_ = 0, win_len_us_ = 0;
    int64_t win_from_pts_ = 0, win_to_pts_ = 0; // stream time_base units, incl. start_time
    int64_t cursor_pts_ = INT64_MIN;            // last decoded frame's pts, or MIN
};

} // namespace mp
