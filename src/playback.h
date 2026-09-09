// Format-agnostic playback engine. Owns the single playback clock + a
// background tick thread; holds a Recording (MCAP, LeRobot, …) and asks it for
// the frames to show as the clock advances. The UI polls latest_frame(topic) /
// current_time_us() / imu_history(topic) each render frame — no signals, no UI
// dependency.
//
// The Lichtblick-style tick loop and the "park on the target frame" seek
// behaviour live here; the codec-specific keyframe/GOP machinery lives in the
// Recording implementation.
#pragma once

#include "recording.h"
#include "video_frame.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mp {

class Playback {
public:
    Playback();
    ~Playback();

    bool open(const std::string& path);
    void close();
    bool is_open() const { return rec_ != nullptr; }
    const std::string& path() const { return path_; }

    void play();
    void pause();
    void toggle();
    void seek(uint64_t timestamp_us);

    void set_speed(float x); // 0.25 .. 8
    float speed() const { return speed_.load(); }

    bool playing() const { return playing_.load(); }
    uint64_t start_time_us() const { return start_us_; }
    uint64_t end_time_us() const { return end_us_; }
    uint64_t current_time_us() const;

    // Playable segments (LeRobot episodes; 1 for MCAP). select() re-preloads
    // and resets the clock to the new segment's start.
    int segment_count() const { return rec_ ? rec_->segment_count() : 0; }
    int current_segment() const { return rec_ ? rec_->current_segment() : 0; }
    SegmentInfo segment_info(int i) const { return rec_ ? rec_->segment_info(i) : SegmentInfo{}; }
    void select_segment(int i);

    // All channel names (video + scalar), sorted.
    const std::vector<std::string>& topics() const { return topics_; }
    // Just the video channels, sorted.
    const std::vector<std::string>& video_topics() const { return video_topics_; }

    // The most-recently-decoded frame for a video channel (may be null).
    VideoFramePtr latest_frame(const std::string& topic);
    // A per-channel running count of frames shown (cheap activity indicator).
    uint64_t message_count(const std::string& topic);
    // Total frames on a video channel (0 for non-video / unknown).
    uint64_t total_message_count(const std::string& topic) const;

    // One IMU sample (accel or gyro), in the file's own units.
    struct ImuSample { uint64_t t_us; double x, y, z; };
    // The whole recording's samples for a scalar channel, oldest first.
    std::vector<ImuSample> imu_history(const std::string& topic);
    ImuSample imu_latest(const std::string& topic);

    // Rolling mono audio: (timestamp, one downsampled amplitude in [-1,1]).
    using AudioPoint = mp::AudioPoint;
    std::vector<AudioPoint> audio_history();
    bool has_audio();

    // A short human-readable summary of a channel's current state.
    std::string latest_summary(const std::string& topic);

    // Per video channel: codec + nominal fps + average bitrate (fixed once the
    // file is scanned) plus the resolution and log time of the frame currently
    // on screen. `valid` is false for an unknown channel.
    struct VideoStats {
        bool valid = false;
        int width = 0, height = 0;
        std::string codec;
        double stream_fps = 0.0;
        double avg_bitrate_bps = 0.0;
        uint64_t frame_ts_us = 0;
        uint64_t frame_count = 0;
    };
    VideoStats video_stats(const std::string& topic);

private:
    void stop_thread();
    void playback_loop();
    // Ask the recording to bring every video channel to `target_us` and merge
    // the result into latest_frames_ (null => blank that channel). Returns
    // false if a newer seek / stop superseded it before it committed.
    bool advance_to(uint64_t target_us);

    std::unique_ptr<Recording> rec_;
    std::string path_;
    uint64_t start_us_ = 0, end_us_ = 0;
    std::vector<std::string> topics_;
    std::vector<std::string> video_topics_;

    std::mutex frames_mutex_;
    std::map<std::string, VideoFramePtr> latest_frames_;
    std::map<std::string, uint64_t> shown_count_;

    std::thread thread_;
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool> seek_pending_{false};
    std::atomic<uint64_t> pending_seek_us_{0};
    // The single playback clock: the log time of the last dispatched batch,
    // plus a small capped wall-time interpolation while playing so the scrubber
    // glides between frames.
    std::atomic<uint64_t> current_time_us_{0};
    std::atomic<int64_t> last_dispatch_ns_{0};
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;
};

} // namespace mp
