#include "playback.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace mp {

Playback::Playback() = default;
Playback::~Playback() { close(); }

void Playback::rebuild_from_rec() {
    start_us_.store(rec_->start_time_us());
    end_us_.store(rec_->end_time_us());
    video_topics_ = rec_->video_channels();
    scalar_topics_ = rec_->scalar_channels();
    topics_ = video_topics_;
    for (const auto& t : scalar_topics_) topics_.push_back(t);
    std::sort(topics_.begin(), topics_.end());
    vinfo_cache_.clear();
    for (const auto& t : video_topics_) vinfo_cache_[t] = rec_->video_info(t);
    segs_.clear();
    for (int i = 0; i < rec_->segment_count(); ++i) segs_.push_back(rec_->segment_info(i));
}

bool Playback::open(const std::string& path) {
    close();
    auto rec = open_recording(path);
    if (!rec) return false;
    rec_ = std::move(rec);
    path_ = path;
    rebuild_from_rec();
    cur_seg_.store(rec_->current_segment());
    seg_switching_.store(false);

    current_time_us_.store(start_us_.load());
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    should_stop_.store(false);
    seek_pending_.store(false);
    playing_.store(false);

    thread_ = std::thread(&Playback::playback_loop, this);
    // Show the current frame right away (Foxglove-style paused preview).
    seek(start_us_.load());
    return true;
}

void Playback::close() {
    ++switch_epoch_; // cancel any in-flight background switch
    if (switch_thread_.joinable()) switch_thread_.join();
    seg_switching_.store(false);
    stop_thread();
    std::lock_guard<std::mutex> rl(rec_mx_);
    rec_.reset();
    path_.clear();
    start_us_.store(0);
    end_us_.store(0);
    topics_.clear();
    video_topics_.clear();
    scalar_topics_.clear();
    segs_.clear();
    vinfo_cache_.clear();
    cur_seg_.store(0);
    std::lock_guard<std::mutex> lk(frames_mutex_);
    latest_frames_.clear();
    shown_count_.clear();
}

void Playback::stop_thread() {
    should_stop_.store(true);
    playing_.store(false);
    pause_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    should_stop_.store(false);
}

void Playback::play() {
    if (!rec_ || playing_.load()) return;
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    playing_.store(true);
    pause_cv_.notify_all();
}

void Playback::pause() { playing_.store(false); }

void Playback::toggle() { playing_.load() ? pause() : play(); }

void Playback::set_speed(float x) { speed_.store(std::clamp(x, 0.25f, 8.0f)); }

void Playback::select_segment(int i) {
    if (!rec_ || i < 0 || i >= (int)segs_.size() || i == cur_seg_.load()) return;

    // Reflect the target immediately so the UI (list highlight, scrubber length,
    // loading bed) updates on the click; the heavy parquet work runs on a
    // background thread so the UI never blocks.
    cur_seg_.store(i);
    const int epoch = ++switch_epoch_;
    {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        latest_frames_.clear();
        shown_count_.clear();
    }
    start_us_.store(0);
    end_us_.store(segs_[i].duration_us);
    current_time_us_.store(0);
    playing_.store(false);

    if (switch_thread_.joinable()) switch_thread_.join();
    seg_switching_.store(true);
    switch_thread_ = std::thread([this, i, epoch] {
        stop_thread();
        if (epoch != switch_epoch_.load()) return; // superseded during stop
        bool ok;
        {
            std::lock_guard<std::mutex> rl(rec_mx_);
            if (epoch != switch_epoch_.load()) return;
            ok = rec_ && rec_->select_segment(i);
            if (ok) rebuild_from_rec();
        }
        if (epoch != switch_epoch_.load()) return;
        current_time_us_.store(start_us_.load());
        seek_pending_.store(false);
        should_stop_.store(false);
        thread_ = std::thread(&Playback::playback_loop, this);
        seek(start_us_.load());
        seg_switching_.store(false);
    });
}

void Playback::seek(uint64_t timestamp_us) {
    if (!rec_) return;
    uint64_t clamped = std::clamp(timestamp_us, start_us_.load(), end_us_.load());
    pending_seek_us_.store(clamped);
    seek_pending_.store(true);
    current_time_us_.store(clamped); // scrubber reflects the target immediately
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    pause_cv_.notify_all();
}

uint64_t Playback::current_time_us() const {
    const uint64_t end = end_us_.load();
    uint64_t base = std::min(current_time_us_.load(), end);
    if (!playing_.load()) return base;
    constexpr int64_t kInterpCapUs = 50'000;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    int64_t wall_us = (now - last_dispatch_ns_.load()) / 1000;
    if (wall_us < 0) wall_us = 0;
    int64_t adv = (int64_t)(wall_us * (double)speed_.load());
    if (adv > kInterpCapUs) adv = kInterpCapUs;
    return std::min(base + (uint64_t)adv, end);
}

bool Playback::advance_to(uint64_t target_us) {
    if (!rec_) return false;
    std::map<std::string, VideoFramePtr> out;
    if (!rec_->seek_video(
            target_us, [this] { return seek_pending_.load() || should_stop_.load(); }, out))
        return false;
    if (should_stop_.load()) return false;
    std::lock_guard<std::mutex> lk(frames_mutex_);
    for (auto& [topic, frame] : out) {
        if (frame) {
            latest_frames_[topic] = frame;
            shown_count_[topic]++;
        } else {
            latest_frames_.erase(topic);
        }
    }
    return true;
}

void Playback::playback_loop() {
    using clock = std::chrono::steady_clock;
    constexpr int64_t kMaxRangeUs = 300'000;
    constexpr auto kTickSleep = std::chrono::milliseconds(8);

    while (!should_stop_.load()) {
        {
            std::unique_lock<std::mutex> lock(pause_mutex_);
            pause_cv_.wait(lock, [this] {
                return playing_.load() || should_stop_.load() || seek_pending_.load();
            });
        }
        if (should_stop_.load()) break;

        if (seek_pending_.exchange(false)) {
            uint64_t t = pending_seek_us_.load();
            current_time_us_.store(t);
            advance_to(t);
            last_dispatch_ns_.store(clock::now().time_since_epoch().count());
        }
        if (!playing_.load()) {
            // Paused (e.g. just after an episode switch): the background loader
            // may still be producing the first frames. Keep re-dispatching the
            // current time until every video channel has a frame, or a short
            // grace period passes.
            auto until = clock::now() + std::chrono::seconds(8);
            size_t last_n = 0;
            int stale = 0;
            while (!playing_.load() && !should_stop_.load() && !seek_pending_.load() &&
                   clock::now() < until) {
                size_t n;
                {
                    std::lock_guard<std::mutex> lk(frames_mutex_);
                    n = latest_frames_.size();
                }
                if (n >= video_topics_.size()) break;
                if (n == last_n && ++stale > 30) break; // ~1.5s with no new frame: give up
                if (n != last_n) stale = 0;
                last_n = n;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                advance_to(current_time_us_.load());
            }
            continue;
        }

        auto last_tick = clock::now();
        double range_ema_us = 16'000.0;

        while (playing_.load() && !should_stop_.load() && !seek_pending_.load()) {
            auto now = clock::now();
            double dt_us =
                (double)std::chrono::duration_cast<std::chrono::microseconds>(now - last_tick)
                    .count();
            last_tick = now;
            double raw = std::min(dt_us * speed_.load(), (double)kMaxRangeUs);
            range_ema_us = range_ema_us * 0.9 + raw * 0.1;

            const uint64_t file_end = end_us_.load();
            uint64_t from = current_time_us_.load();
            uint64_t to = from + (uint64_t)std::max(1.0, range_ema_us);
            bool hit_end = to >= file_end;
            if (hit_end) to = file_end;

            advance_to(to);
            if (seek_pending_.load() || should_stop_.load()) break;
            current_time_us_.store(to);
            last_dispatch_ns_.store(clock::now().time_since_epoch().count());

            if (hit_end) {
                playing_.store(false);
                break;
            }
            std::this_thread::sleep_for(kTickSleep);
        }
    }
}

VideoFramePtr Playback::latest_frame(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = latest_frames_.find(topic);
    return it == latest_frames_.end() ? nullptr : it->second;
}

uint64_t Playback::message_count(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = shown_count_.find(topic);
    return it == shown_count_.end() ? 0 : it->second;
}

uint64_t Playback::total_message_count(const std::string& topic) const {
    auto it = vinfo_cache_.find(topic);
    return it == vinfo_cache_.end() ? 0 : it->second.frame_count;
}

std::vector<Playback::ImuSample> Playback::imu_history(const std::string& topic) {
    if (!rec_ || seg_switching_.load()) return {};
    std::lock_guard<std::mutex> rl(rec_mx_);
    if (!rec_) return {};
    const auto& src = rec_->scalar_history(topic);
    std::vector<ImuSample> out;
    out.reserve(src.size());
    for (const auto& s : src)
        out.push_back({s.t_us, s.v.size() > 0 ? (double)s.v[0] : 0.0,
                       s.v.size() > 1 ? (double)s.v[1] : 0.0,
                       s.v.size() > 2 ? (double)s.v[2] : 0.0});
    return out;
}

Playback::ImuSample Playback::imu_latest(const std::string& topic) {
    if (!rec_ || seg_switching_.load()) return {0, 0, 0, 0};
    std::lock_guard<std::mutex> rl(rec_mx_);
    if (!rec_) return {0, 0, 0, 0};
    const auto& src = rec_->scalar_history(topic);
    if (src.empty()) return {0, 0, 0, 0};
    const auto& s = src.back();
    return {s.t_us, s.v.size() > 0 ? (double)s.v[0] : 0.0, s.v.size() > 1 ? (double)s.v[1] : 0.0,
            s.v.size() > 2 ? (double)s.v[2] : 0.0};
}

Playback::ScalarSeries Playback::scalar_series(const std::string& topic) {
    ScalarSeries out;
    if (!rec_ || seg_switching_.load()) return out;
    std::lock_guard<std::mutex> rl(rec_mx_);
    if (!rec_) return out;
    ScalarChannelInfo ci = rec_->scalar_info(topic);
    out.name = ci.display_name;
    out.dims = ci.dims;
    out.labels = ci.dim_labels;
    out.samples = rec_->scalar_history(topic); // copy — small, immutable after preload
    return out;
}

std::string Playback::latest_summary(const std::string& topic) {
    VideoStats vs = video_stats(topic);
    if (!vs.valid) return {};
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%dx%d  %s  %.0f fps", vs.width, vs.height, vs.codec.c_str(),
                  vs.stream_fps);
    return buf;
}

Playback::VideoStats Playback::video_stats(const std::string& topic) {
    VideoStats s;
    auto it = vinfo_cache_.find(topic);
    if (it == vinfo_cache_.end() || !it->second.valid) return s;
    const VideoChannelInfo& ci = it->second;
    s.valid = true;
    s.codec = ci.codec;
    s.frame_count = ci.frame_count;
    s.width = ci.width;
    s.height = ci.height;
    s.stream_fps = ci.fps;
    s.avg_bitrate_bps = ci.bitrate_bps;
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto fit = latest_frames_.find(topic);
    if (fit != latest_frames_.end() && fit->second) {
        s.width = fit->second->width;
        s.height = fit->second->height;
        s.frame_ts_us = fit->second->timestamp_us;
    }
    return s;
}

std::vector<Playback::AudioPoint> Playback::audio_history() {
    if (!rec_ || seg_switching_.load()) return {};
    std::lock_guard<std::mutex> rl(rec_mx_);
    return rec_ ? rec_->audio_history() : std::vector<AudioPoint>{};
}

bool Playback::has_audio() { return rec_ && rec_->has_audio(); }

} // namespace mp
