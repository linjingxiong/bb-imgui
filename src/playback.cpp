#include "playback.h"

#include "mcap_reader.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>

namespace mp {

namespace {
bool is_video_topic(const std::string& t) { return t.rfind("/camera/", 0) == 0; }
bool is_imu_topic(const std::string& t) { return t.rfind("/imu/", 0) == 0; }
} // namespace

Playback::Playback() = default;
Playback::~Playback() { close(); }

bool Playback::open(const std::string& path) {
    close();
    if (!reader_.open(path)) return false;
    path_ = path;

    auto topic_set = reader_.topics_with_messages();
    topics_.assign(topic_set.begin(), topic_set.end());
    std::sort(topics_.begin(), topics_.end());
    msg_totals_ = reader_.message_totals();
    video_topics_.clear();
    for (const auto& t : topics_)
        if (is_video_topic(t)) video_topics_.push_back(t);

    for (const auto& t : video_topics_)
        decoders_[t] = std::make_unique<VideoDecoder>();

    preload_history();

    current_time_us_.store(reader_.start_time_us());
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    should_stop_.store(false);
    seek_pending_.store(false);
    playing_.store(false);

    thread_ = std::thread(&Playback::playback_loop, this);
    // Decode the frame at the start position right away so the video panels
    // aren't blank before the first play (Foxglove shows the current frame
    // even while paused).
    seek(reader_.start_time_us());
    return true;
}

void Playback::close() {
    stop_thread();
    reader_.close();
    decoders_.clear();
    path_.clear();
    topics_.clear();
    video_topics_.clear();
    history_preloaded_.store(false);
    std::lock_guard<std::mutex> lk(frames_mutex_);
    latest_frames_.clear();
    msg_counts_.clear();
    msg_totals_.clear();
    imu_hist_.clear();
    audio_hist_.clear();
    has_audio_ = false;
    latest_summary_.clear();
}

void Playback::preload_history() {
    history_preloaded_.store(false);
    reader_.read_messages(reader_.start_time_us(), 0, [&](const McapMessage& m) -> bool {
        if (should_stop_.load()) return false;
        if (is_imu_topic(m.topic) || m.topic == "/audio") dispatch(m);
        return true;
    });
    // dispatch() also bumps msg_counts_; the preload scan isn't playback, so
    // reset them and let the playback thread re-accumulate as it actually runs.
    {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        msg_counts_.clear();
    }
    history_preloaded_.store(true);
}

void Playback::stop_thread() {
    should_stop_.store(true);
    playing_.store(false);
    pause_cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    should_stop_.store(false);
}

void Playback::play() {
    if (!reader_.is_open() || playing_.load()) return;
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    playing_.store(true);
    pause_cv_.notify_all();
}

void Playback::pause() { playing_.store(false); }

void Playback::toggle() { playing_.load() ? pause() : play(); }

void Playback::set_speed(float x) { speed_.store(std::clamp(x, 0.25f, 8.0f)); }

void Playback::seek(uint64_t timestamp_us) {
    if (!reader_.is_open()) return;
    uint64_t clamped = std::clamp(timestamp_us, reader_.start_time_us(), reader_.end_time_us());
    pending_seek_us_.store(clamped);
    seek_pending_.store(true);
    current_time_us_.store(clamped); // scrubber reflects the target immediately
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    pause_cv_.notify_all();
}

uint64_t Playback::current_time_us() const {
    uint64_t base = std::min(current_time_us_.load(), reader_.end_time_us());
    if (!playing_.load()) return base;
    // While playing, glide up to one frame ahead of the last dispatched
    // message so the scrubber isn't stepped; never further (no drift).
    constexpr int64_t kInterpCapUs = 50'000;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    int64_t wall_us = (now - last_dispatch_ns_.load()) / 1000;
    if (wall_us < 0) wall_us = 0;
    int64_t adv = (int64_t)(wall_us * (double)speed_.load());
    if (adv > kInterpCapUs) adv = kInterpCapUs;
    return std::min(base + (uint64_t)adv, reader_.end_time_us());
}

VideoFramePtr Playback::latest_frame(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = latest_frames_.find(topic);
    return it == latest_frames_.end() ? nullptr : it->second;
}

uint64_t Playback::message_count(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = msg_counts_.find(topic);
    return it == msg_counts_.end() ? 0 : it->second;
}

uint64_t Playback::total_message_count(const std::string& topic) const {
    auto it = msg_totals_.find(topic);
    return it == msg_totals_.end() ? 0 : it->second;
}

std::vector<Playback::ImuSample> Playback::imu_history(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = imu_hist_.find(topic);
    if (it == imu_hist_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

Playback::ImuSample Playback::imu_latest(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = imu_hist_.find(topic);
    if (it == imu_hist_.end() || it->second.empty()) return {0, 0, 0, 0};
    return it->second.back();
}

std::string Playback::latest_summary(const std::string& topic) {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    auto it = latest_summary_.find(topic);
    return it == latest_summary_.end() ? std::string() : it->second;
}

std::vector<Playback::AudioPoint> Playback::audio_history() {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    return {audio_hist_.begin(), audio_hist_.end()};
}

bool Playback::has_audio() {
    std::lock_guard<std::mutex> lk(frames_mutex_);
    return has_audio_;
}

void Playback::dispatch(const McapMessage& msg) {
    {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        msg_counts_[msg.topic]++;
    }

    if (is_imu_topic(msg.topic)) {
        DecodedImuSample s;
        if (!decode_imu_sample(msg.data, s)) return;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "x %.4f   y %.4f   z %.4f", s.x, s.y, s.z);
        std::lock_guard<std::mutex> lk(frames_mutex_);
        latest_summary_[msg.topic] = buf;
        if (!history_preloaded_.load()) {
            auto& dq = imu_hist_[msg.topic];
            dq.push_back({s.timestamp_us ? s.timestamp_us : msg.timestamp_us, s.x, s.y, s.z});
            while (dq.size() > kImuHistCap) dq.pop_front();
        }
        return;
    }

    if (msg.topic == "/audio") {
        DecodedRawAudio a;
        if (!decode_raw_audio(msg.data, a)) return;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s  %u Hz  %u ch  %zu bytes", a.format.c_str(),
                      a.sample_rate, a.channels, a.data.size());
        // PCM s16le -> a handful of downsampled mono amplitudes per chunk so
        // the waveform panel has something continuous to draw.
        const int ch = a.channels ? (int)a.channels : 1;
        const int16_t* s = reinterpret_cast<const int16_t*>(a.data.data());
        const size_t frames = a.data.size() / (2 * ch);
        const int buckets = 32;
        std::lock_guard<std::mutex> lk(frames_mutex_);
        latest_summary_[msg.topic] = buf;
        has_audio_ = true;
        if (!history_preloaded_.load()) {
            uint32_t sr = a.sample_rate ? a.sample_rate : 16000;
            for (int b = 0; b < buckets && frames > 0; ++b) {
                size_t f0 = frames * b / buckets, f1 = frames * (b + 1) / buckets;
                float peak = 0;
                for (size_t f = f0; f < f1; ++f) {
                    float v = s[f * ch] / 32768.0f;
                    peak = std::max(peak, v < 0 ? -v : v);
                }
                uint64_t t = msg.timestamp_us + (uint64_t)((double)f0 / sr * 1e6);
                audio_hist_.push_back({t, peak});
            }
            while (audio_hist_.size() > kAudioHistCap) audio_hist_.pop_front();
        }
        return;
    }

    if (!is_video_topic(msg.topic)) {
        std::lock_guard<std::mutex> lk(frames_mutex_);
        latest_summary_[msg.topic] = msg.schema_name.empty() ? msg.message_encoding
                                                             : msg.schema_name;
        return;
    }

    auto dit = decoders_.find(msg.topic);
    if (dit == decoders_.end()) return;

    DecodedCompressedVideo v;
    if (!decode_compressed_video(msg.data, v)) return;

    auto frame = dit->second->decode(v.format, v.data.data(), (int)v.data.size());
    if (frame) {
        frame->timestamp_us = v.timestamp_us;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%dx%d  %s  '%s'", frame->width, frame->height,
                      v.format.c_str(), v.frame_id.c_str());
        std::lock_guard<std::mutex> lk(frames_mutex_);
        latest_frames_[msg.topic] = frame;
        latest_summary_[msg.topic] = buf;
    }
}

void Playback::do_seek_catchup(uint64_t target_us) {
    for (auto& [_, d] : decoders_) d->reset();
    if (!history_preloaded_.load()) {
        // Rebuild IMU history cleanly for the window around the new position
        // (a backward seek would otherwise leave stale future samples).
        // When the whole track is preloaded it's immutable — leave it alone.
        std::lock_guard<std::mutex> lk(frames_mutex_);
        imu_hist_.clear();
        audio_hist_.clear();
    }
    uint64_t from_us = reader_.seekable_start_time_us(target_us);
    if (from_us > target_us) from_us = target_us;

    // Run the whole window through the decoder without emitting a single
    // intermediate frame (decode_discard advances the P-frame chain only).
    // For each video topic keep the last packet at or before the target
    // (or, near the file start, the first packet after it); decode just
    // that one at the end so the panel updates exactly once.
    const uint64_t tail_fwd_us = 120'000;
    std::map<std::string, DecodedCompressedVideo> want; // topic -> packet to show

    reader_.read_messages(from_us, target_us + tail_fwd_us, [&](const McapMessage& m) -> bool {
        if (should_stop_.load() || seek_pending_.load()) return false;
        if (!is_video_topic(m.topic)) {
            dispatch(m); // summary / (non-preloaded) imu+audio
            return true;
        }
        auto dit = decoders_.find(m.topic);
        if (dit == decoders_.end()) return true;
        DecodedCompressedVideo v;
        if (!decode_compressed_video(m.data, v)) return true;
        if (v.timestamp_us <= target_us) {
            auto it = want.find(m.topic);
            if (it != want.end())
                dit->second->decode_discard(it->second.format, it->second.data.data(),
                                            (int)it->second.data.size());
            want[m.topic] = std::move(v);
        } else if (want.find(m.topic) == want.end()) {
            want[m.topic] = std::move(v); // nothing before target — fall back to the next frame
        }
        return true;
    });

    for (auto& [topic, v] : want) {
        auto dit = decoders_.find(topic);
        if (dit == decoders_.end()) continue;
        auto frame = dit->second->decode(v.format, v.data.data(), (int)v.data.size());
        if (!frame) continue;
        frame->timestamp_us = v.timestamp_us;
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%dx%d  %s  '%s'", frame->width, frame->height,
                      v.format.c_str(), v.frame_id.c_str());
        std::lock_guard<std::mutex> lk(frames_mutex_);
        latest_frames_[topic] = frame;
        latest_summary_[topic] = buf;
    }
}

void Playback::playback_loop() {
    using clock = std::chrono::steady_clock;
    constexpr int64_t kMinSleepUs = 2000;
    constexpr int64_t kMaxGapUs = 2'000'000;

    while (!should_stop_.load()) {
        {
            std::unique_lock<std::mutex> lock(pause_mutex_);
            pause_cv_.wait(lock, [this] {
                return playing_.load() || should_stop_.load() || seek_pending_.load();
            });
        }
        if (should_stop_.load()) break;

        uint64_t start_us = current_time_us_.load();
        if (seek_pending_.exchange(false)) {
            start_us = pending_seek_us_.load();
            current_time_us_.store(start_us);
            do_seek_catchup(start_us);
        }
        if (!playing_.load()) continue;

        auto wall_anchor = clock::now();
        uint64_t rec_anchor_us = start_us;
        bool interrupted = false;

        reader_.read_messages(start_us, 0, [&](const McapMessage& msg) -> bool {
            if (should_stop_.load() || seek_pending_.load()) { interrupted = true; return false; }
            bool was_paused = false;
            while (!playing_.load()) {
                was_paused = true;
                if (should_stop_.load() || seek_pending_.load()) { interrupted = true; return false; }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            if (was_paused) {
                wall_anchor = clock::now();
                rec_anchor_us = msg.timestamp_us;
            }
            int64_t rec_elapsed_us = (int64_t)msg.timestamp_us - (int64_t)rec_anchor_us;
            if (rec_elapsed_us > kMaxGapUs || rec_elapsed_us < 0) {
                wall_anchor = clock::now();
                rec_anchor_us = msg.timestamp_us;
                rec_elapsed_us = 0;
            }
            int64_t wall_us = (int64_t)(rec_elapsed_us / speed_.load());
            auto target = wall_anchor + std::chrono::microseconds(wall_us);
            auto now = clock::now();
            if (target - now >= std::chrono::microseconds(kMinSleepUs))
                std::this_thread::sleep_for(target - now);

            dispatch(msg);
            current_time_us_.store(msg.timestamp_us);
            last_dispatch_ns_.store(clock::now().time_since_epoch().count());
            return true;
        });

        if (!interrupted && !should_stop_.load() && !seek_pending_.load()) {
            current_time_us_.store(reader_.end_time_us()); // reached end of file
            playing_.store(false);
        }
    }
}

} // namespace mp
