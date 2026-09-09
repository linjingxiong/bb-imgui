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

// Does this compressed packet start a fresh GOP (an IDR / IRAP, or carries a
// parameter set)? Handles both Annex-B (00 00 01 start codes) and 4-byte
// length-prefixed framing, H.264 and H.265.
bool packet_is_keyframe(const std::string& fmt, const uint8_t* d, size_t n) {
    const bool h265 = fmt.find("265") != std::string::npos || fmt.find("hevc") != std::string::npos;
    auto nal_is_key = [&](const uint8_t* p, size_t len) {
        if (len < 1) return false;
        if (h265) {
            int t = (p[0] >> 1) & 0x3F;
            return (t >= 16 && t <= 21) || t == 32 || t == 33; // BLA/IDR/CRA, VPS, SPS
        }
        int t = p[0] & 0x1F;
        return t == 5 || t == 7; // IDR slice, SPS
    };
    const bool annexb = n >= 4 && d[0] == 0 && d[1] == 0 &&
                        (d[2] == 1 || (d[2] == 0 && d[3] == 1));
    if (annexb) {
        for (size_t p = 0; p + 3 < n;) {
            if (d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1) {
                if (nal_is_key(d + p + 3, n - p - 3)) return true;
                p += 3;
            } else if (p + 4 < n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1) {
                if (nal_is_key(d + p + 4, n - p - 4)) return true;
                p += 4;
            } else {
                ++p;
            }
        }
        return false;
    }
    for (size_t i = 0; i + 4 <= n;) {
        uint32_t len = ((uint32_t)d[i] << 24) | ((uint32_t)d[i + 1] << 16) |
                       ((uint32_t)d[i + 2] << 8) | d[i + 3];
        i += 4;
        if (len == 0 || i + len > n) break;
        if (nal_is_key(d + i, len)) return true;
        i += len;
    }
    return false;
}

// Pull the Annex-B parameter-set NALs (SPS/PPS, plus VPS for H.265), each with
// its start code, out of a packet — usable as decoder extradata. Empty if the
// packet carries no SPS.
std::vector<uint8_t> extract_param_sets(const std::string& fmt, const uint8_t* d, size_t n) {
    const bool h265 = fmt.find("265") != std::string::npos || fmt.find("hevc") != std::string::npos;
    auto is_param = [&](uint8_t b0) {
        int t = h265 ? ((b0 >> 1) & 0x3F) : (b0 & 0x1F);
        return h265 ? (t == 32 || t == 33 || t == 34) : (t == 7 || t == 8);
    };
    if (!(n >= 4 && d[0] == 0 && d[1] == 0 && (d[2] == 1 || (d[2] == 0 && d[3] == 1))))
        return {};
    std::vector<uint8_t> out;
    bool has_sps = false;
    for (size_t p = 0; p + 3 < n;) {
        size_t sc = 0;
        if (d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1)
            sc = 3;
        else if (p + 4 < n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1)
            sc = 4;
        if (sc == 0) { ++p; continue; }
        size_t nal = p + sc;
        // find the next start code = end of this NAL
        size_t e = nal;
        for (; e + 3 <= n; ++e)
            if (d[e] == 0 && d[e + 1] == 0 && (d[e + 2] == 1 || (e + 3 < n && d[e + 3] == 1)))
                break;
        if (e + 3 > n) e = n;
        if (nal < n && is_param(d[nal])) {
            int t = h265 ? ((d[nal] >> 1) & 0x3F) : (d[nal] & 0x1F);
            if ((h265 && t == 33) || (!h265 && t == 7)) has_sps = true;
            out.insert(out.end(), d + p, d + e);
        }
        p = e;
    }
    return has_sps ? out : std::vector<uint8_t>{};
}
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

    for (const auto& t : video_topics_) {
        decoders_[t] = std::make_unique<VideoDecoder>();
        decoder_pos_us_[t] = 0;
    }

    preload_history();

    current_time_us_.store(reader_.start_time_us());
    last_dispatch_ns_.store(std::chrono::steady_clock::now().time_since_epoch().count());
    should_stop_.store(false);
    seek_pending_.store(false);
    playing_.store(false);

    thread_ = std::thread(&Playback::playback_loop, this);
    // Show the current frame right away (Foxglove-style paused preview). Any
    // camera still inside its undecodable opening GOP stays black until the
    // scrubber passes its first keyframe.
    seek(reader_.start_time_us());
    return true;
}

void Playback::close() {
    stop_thread();
    reader_.close();
    decoders_.clear();
    decoder_pos_us_.clear();
    path_.clear();
    topics_.clear();
    video_topics_.clear();
    history_preloaded_.store(false);
    video_keyframes_.clear();
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
    video_keyframes_.clear();
    reader_.read_messages(reader_.start_time_us(), 0, [&](const McapMessage& m) -> bool {
        if (should_stop_.load()) return false;
        if (is_imu_topic(m.topic) || m.topic == "/audio") {
            dispatch(m);
        } else if (is_video_topic(m.topic)) {
            DecodedCompressedVideo v;
            if (decode_compressed_video(m.data, v) && !v.data.empty()) {
                if (packet_is_keyframe(v.format, v.data.data(), v.data.size()))
                    video_keyframes_[m.topic].push_back(m.timestamp_us);
                auto dit = decoders_.find(m.topic);
                if (dit != decoders_.end() && dit->second) {
                    auto ex = extract_param_sets(v.format, v.data.data(), v.data.size());
                    if (!ex.empty()) dit->second->set_extradata(ex.data(), (int)ex.size());
                }
            }
        }
        return true;
    });
    for (auto& [_, kfs] : video_keyframes_) std::sort(kfs.begin(), kfs.end());
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

    // Video topics never reach here — present_video_batch() owns them.
    std::lock_guard<std::mutex> lk(frames_mutex_);
    latest_summary_[msg.topic] =
        msg.schema_name.empty() ? msg.message_encoding : msg.schema_name;
}

std::map<std::string, uint64_t> Playback::present_video_batch(
    uint64_t read_start_us, uint64_t read_end_us,
    const std::map<std::string, TopicReplay>* plan) {

    // Phase 1 — read the window once. Bucket each video topic's packets in log
    // order; dispatch non-video inline. Mutates no decoder state, so an abort
    // here (a newer seek arrived) is a clean no-op.
    std::map<std::string, std::vector<DecodedCompressedVideo>> packets;
    reader_.read_messages(read_start_us, read_end_us, [&](const McapMessage& m) -> bool {
        if (should_stop_.load() || seek_pending_.load()) return false;
        if (!is_video_topic(m.topic)) {
            dispatch(m);
            return true;
        }
        if (plan) {
            auto it = plan->find(m.topic);
            if (it == plan->end() || m.timestamp_us < it->second.start_us) return true;
        }
        {
            std::lock_guard<std::mutex> lk(frames_mutex_);
            msg_counts_[m.topic]++;
        }
        if (decoders_.find(m.topic) == decoders_.end()) return true;
        DecodedCompressedVideo v;
        if (!decode_compressed_video(m.data, v)) return true;
        packets[m.topic].push_back(std::move(v));
        return true;
    });

    std::map<std::string, uint64_t> shown;
    if (should_stop_.load() || seek_pending_.load()) return shown;

    // Phase 2 — replay each topic's chain: (optionally flush first, then)
    // decode_discard everything but the last packet (cutting corners on those,
    // see set_fast_replay), decode the last one at full quality. Independent
    // decoders, so run the topics on separate threads when at least one has a
    // real GOP to grind through.
    struct Out { VideoFramePtr frame; std::string summary; uint64_t ts = 0; };
    std::vector<std::pair<std::string, std::vector<DecodedCompressedVideo>*>> work;
    bool heavy = false;
    for (auto& [topic, pkts] : packets) {
        if (pkts.empty()) continue;
        work.push_back({topic, &pkts});
        if (pkts.size() > 1) heavy = true;
    }
    std::map<std::string, Out> outs;

    auto replay = [&](const std::string& topic, std::vector<DecodedCompressedVideo>& pkts) -> Out {
        VideoDecoder* dec = decoders_.find(topic)->second.get();
        // Key exists for every video topic (created in open()); each thread
        // writes a distinct element, so the map access is race-free.
        uint64_t& pos = decoder_pos_us_.find(topic)->second;
        if (plan) {
            auto it = plan->find(topic);
            if (it != plan->end() && it->second.flush) { dec->flush(); pos = 0; }
        }
        dec->set_fast_replay(true);
        for (size_t i = 0; i + 1 < pkts.size(); ++i)
            dec->decode_discard(pkts[i].format, pkts[i].data.data(), (int)pkts[i].data.size());
        dec->set_fast_replay(false);
        DecodedCompressedVideo& v = pkts.back();
        Out o;
        auto frame = dec->decode(v.format, v.data.data(), (int)v.data.size());
        if (frame) {
            pos = v.timestamp_us; // decoder consumed up to here AND we painted it
            frame->timestamp_us = v.timestamp_us;
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%dx%d  %s  '%s'", frame->width, frame->height,
                          v.format.c_str(), v.frame_id.c_str());
            o.frame = std::move(frame);
            o.summary = buf;
            o.ts = v.timestamp_us;
        } else {
            // Decode failed: the decoder dropped its references internally, so
            // our tracked position is no longer trustworthy — force the next
            // catch-up to replay from a keyframe.
            pos = 0;
        }
        return o;
    };

    if (heavy && work.size() > 1) {
        std::vector<std::thread> workers;
        std::mutex omx;
        for (auto& [topic, pv] : work) {
            workers.emplace_back([&, topic, pv] {
                Out o = replay(topic, *pv);
                std::lock_guard<std::mutex> lk(omx);
                outs.emplace(topic, std::move(o));
            });
        }
        for (auto& w : workers) w.join();
    } else {
        for (auto& [topic, pv] : work) outs.emplace(topic, replay(topic, *pv));
    }

    // Phase 3 — publish. Always paint a completed batch (unless shutting down):
    // that keeps decoder position == painted frame for every topic, the
    // invariant the incremental-vs-flush decision in do_seek_catchup relies on.
    if (should_stop_.load()) return shown;
    std::lock_guard<std::mutex> lk(frames_mutex_);
    for (auto& [topic, o] : outs) {
        if (!o.frame) continue;
        latest_frames_[topic] = o.frame;
        latest_summary_[topic] = o.summary;
        shown[topic] = o.ts;
    }
    return shown;
}

void Playback::do_seek_catchup(uint64_t target_us) {
    // A newer seek is already queued — skip the expensive work and let the
    // playback loop come round again for the latest target.
    if (seek_pending_.load() || should_stop_.load()) return;

    if (!history_preloaded_.load()) {
        // Rebuild IMU history cleanly for the window around the new position
        // (a backward seek would otherwise leave stale future samples).
        // When the whole track is preloaded it's immutable — leave it alone.
        std::lock_guard<std::mutex> lk(frames_mutex_);
        imu_hist_.clear();
        audio_hist_.clear();
    }

    // Plan each camera independently:
    //  - no keyframe at or before the target → the target is inside this
    //    camera's undecodable opening GOP; blank the panel, nothing to replay;
    //  - decoder already sits past this target's keyframe and before the target
    //    → decode forward from there (a small step — the drag-right case);
    //  - otherwise flush and replay from the keyframe at or before the target.
    // The flush itself is deferred into present_video_batch so a superseded seek
    // touches nothing.
    std::map<std::string, TopicReplay> plan;
    uint64_t read_start = target_us;
    for (auto& [topic, dec] : decoders_) {
        auto kit = video_keyframes_.find(topic);
        bool have_kf = kit != video_keyframes_.end() && !kit->second.empty() &&
                       kit->second.front() <= target_us;
        if (!have_kf) {
            dec->flush();
            decoder_pos_us_.find(topic)->second = 0;
            std::lock_guard<std::mutex> lk(frames_mutex_);
            latest_frames_.erase(topic);
            latest_summary_.erase(topic);
            continue;
        }
        uint64_t kf = *(std::upper_bound(kit->second.begin(), kit->second.end(), target_us) - 1);
        uint64_t cur = decoder_pos_us_.find(topic)->second;

        TopicReplay tr;
        if (cur > 0 && cur >= kf && cur < target_us)
            tr = {cur + 1, false}; // forward within the current GOP — no replay
        else
            tr = {kf, true};
        plan[topic] = tr;
        read_start = std::min(read_start, tr.start_us);
    }

    present_video_batch(read_start, target_us + 1, &plan);
}

void Playback::playback_loop() {
    using clock = std::chrono::steady_clock;
    // Lichtblick-style tick loop: advance the clock by (wall time since last
    // tick x speed), capped, and hand the whole message range to
    // present_video_batch at once (no per-message pacing) — so simultaneous
    // frames from different topics reach the UI together, and a range spanning
    // several frames paints only the last one.
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
            do_seek_catchup(t);
            last_dispatch_ns_.store(clock::now().time_since_epoch().count());
        }
        if (!playing_.load()) continue;

        auto last_tick = clock::now();
        double range_ema_us = 16'000.0;
        const uint64_t file_end = reader_.end_time_us();

        while (playing_.load() && !should_stop_.load() && !seek_pending_.load()) {
            auto now = clock::now();
            double dt_us =
                (double)std::chrono::duration_cast<std::chrono::microseconds>(now - last_tick).count();
            last_tick = now;
            double raw = std::min(dt_us * speed_.load(), (double)kMaxRangeUs);
            range_ema_us = range_ema_us * 0.9 + raw * 0.1;

            uint64_t from = current_time_us_.load();
            uint64_t to = from + (uint64_t)std::max(1.0, range_ema_us);
            bool hit_end = to >= file_end;
            if (hit_end) to = file_end;

            present_video_batch(from + 1, to + 1);
            // A seek arrived mid-batch: don't commit this tick's end time —
            // the seek handler owns the clock now.
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

} // namespace mp
