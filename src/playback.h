// Simplified, Qt-free port of EgoViewer's PlaybackService. Owns the reader
// + one VideoDecoder per video topic + a playback thread. The UI polls
// latest_frame(topic) / current_time_us() / etc. each render frame — no
// signals/slots, no AppDataStore. IMU/audio history is deferred to a later
// batch; for now only video + the timeline are wired up.
//
// The playback thread's wall-clock-anchored pacing and the reset-decoder /
// decode-discard-to-target seek catch-up are carried over from EgoViewer
// (that's the part that was actually hard to get right — see its comments).
#pragma once

#include "mcap_reader.h"
#include "video_frame.h"
#include "video_decoder.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
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
    bool is_open() const { return reader_.is_open(); }
    const std::string& path() const { return path_; }

    void play();
    void pause();
    void toggle();
    void seek(uint64_t timestamp_us);

    void set_speed(float x); // 0.25 .. 8
    float speed() const { return speed_.load(); }

    bool playing() const { return playing_.load(); }
    uint64_t start_time_us() const { return reader_.start_time_us(); }
    uint64_t end_time_us() const { return reader_.end_time_us(); }
    uint64_t current_time_us() const;

    // All topics in the file (video + non-video), sorted.
    const std::vector<std::string>& topics() const { return topics_; }
    // Just the "/camera/*" topics (h264/h265 CompressedVideo), sorted.
    const std::vector<std::string>& video_topics() const { return video_topics_; }

    // The most-recently-decoded frame for a video topic (may be null).
    VideoFramePtr latest_frame(const std::string& topic);
    // A per-topic running total of how many messages have been dispatched
    // (cheap "activity" indicator for the topic tree).
    uint64_t message_count(const std::string& topic);
    // Total messages on `topic` in the whole file (from the summary; 0 if
    // unknown).
    uint64_t total_message_count(const std::string& topic) const;

    // One IMU sample (accel or gyro), in the file's own units.
    struct ImuSample { uint64_t t_us; double x, y, z; };
    // A rolling window of recent samples for an "/imu/*" topic (oldest
    // first, capped). Copy-returned — safe to iterate without holding a lock.
    std::vector<ImuSample> imu_history(const std::string& topic);
    ImuSample imu_latest(const std::string& topic);

    // Rolling mono audio: (timestamp, one downsampled amplitude in [-1,1]).
    struct AudioPoint { uint64_t t_us; float amp; };
    std::vector<AudioPoint> audio_history();
    bool has_audio();

    // A short human-readable summary of the latest message on `topic`
    // (values for IMU, WxH/codec/frame_id for video, format/rate for audio).
    std::string latest_summary(const std::string& topic);

private:
    void stop_thread();
    void playback_loop();
    void dispatch(const McapMessage& msg);
    void do_seek_catchup(uint64_t target_us);
    // Scan the whole file once at open() and pull every /imu/* + /audio sample
    // into imu_hist_/audio_hist_ (they're tiny), so the sensor plots can show
    // the entire recording at once (Foxglove recorded-playback style) rather
    // than a rolling window fed by the playback thread.
    void preload_history();

    McapReader reader_;
    std::string path_;
    std::vector<std::string> topics_;
    std::vector<std::string> video_topics_;

    std::map<std::string, std::unique_ptr<VideoDecoder>> decoders_;

    std::mutex frames_mutex_;
    std::map<std::string, VideoFramePtr> latest_frames_;
    std::map<std::string, uint64_t> msg_counts_;
    std::map<std::string, uint64_t> msg_totals_; // from the file summary
    std::map<std::string, std::deque<ImuSample>> imu_hist_;
    std::deque<AudioPoint> audio_hist_;
    bool has_audio_ = false;
    std::map<std::string, std::string> latest_summary_;
    // Whole recording is preloaded (see preload_history); once set, the
    // playback thread stops appending to imu_hist_/audio_hist_ and seeks no
    // longer clear them. Big caps because the deques now hold the full track.
    std::atomic<bool> history_preloaded_{false};
    static constexpr size_t kImuHistCap = 5'000'000;
    static constexpr size_t kAudioHistCap = 5'000'000;

    std::thread thread_;
    std::atomic<bool> should_stop_{false};
    std::atomic<bool> playing_{false};
    std::atomic<float> speed_{1.0f};
    std::atomic<bool> seek_pending_{false};
    std::atomic<uint64_t> pending_seek_us_{0};
    // The single playback clock: the log time of the last dispatched message.
    // current_time_us() reads this (plus a small capped wall-time interpolation
    // while playing, so the scrubber glides between frames). No separate
    // free-running virtual clock — the dispatch loop is the source of truth.
    std::atomic<uint64_t> current_time_us_{0};
    std::atomic<int64_t> last_dispatch_ns_{0}; // steady_clock ns at that dispatch
    std::mutex pause_mutex_;
    std::condition_variable pause_cv_;

    bool suppress_catchup_display_ = false;
};

} // namespace mp
