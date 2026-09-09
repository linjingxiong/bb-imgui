#include "mcap_recording.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <thread>

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

McapRecording::~McapRecording() {
    decoders_.clear();
    reader_.close();
}

bool McapRecording::open(const std::string& path) {
    if (!reader_.open(path)) return false;

    auto topic_set = reader_.topics_with_messages();
    topics_.assign(topic_set.begin(), topic_set.end());
    std::sort(topics_.begin(), topics_.end());
    for (const auto& t : topics_) {
        if (is_video_topic(t)) video_topics_.push_back(t);
        else if (is_imu_topic(t)) scalar_topics_.push_back(t);
    }
    for (const auto& t : video_topics_) {
        decoders_[t] = std::make_unique<VideoDecoder>();
        decoder_pos_us_[t] = 0;
    }
    preload();
    return true;
}

void McapRecording::preload() {
    reader_.read_messages(reader_.start_time_us(), 0, [&](const McapMessage& m) -> bool {
        if (is_imu_topic(m.topic)) {
            DecodedImuSample s;
            if (!decode_imu_sample(m.data, s)) return true;
            ScalarSample ss;
            ss.t_us = s.timestamp_us ? s.timestamp_us : m.timestamp_us;
            ss.dims = 3;
            ss.v[0] = (float)s.x;
            ss.v[1] = (float)s.y;
            ss.v[2] = (float)s.z;
            scalar_hist_[m.topic].push_back(ss);
        } else if (m.topic == "/audio") {
            DecodedRawAudio a;
            if (!decode_raw_audio(m.data, a)) return true;
            has_audio_ = true;
            const int ch = a.channels ? (int)a.channels : 1;
            const int16_t* s = reinterpret_cast<const int16_t*>(a.data.data());
            const size_t frames = a.data.size() / (2 * ch);
            const int buckets = 32;
            uint32_t sr = a.sample_rate ? a.sample_rate : 16000;
            for (int b = 0; b < buckets && frames > 0; ++b) {
                size_t f0 = frames * b / buckets, f1 = frames * (b + 1) / buckets;
                float peak = 0;
                for (size_t f = f0; f < f1; ++f) {
                    float v = s[f * ch] / 32768.0f;
                    peak = std::max(peak, v < 0 ? -v : v);
                }
                audio_hist_.push_back({m.timestamp_us + (uint64_t)((double)f0 / sr * 1e6), peak});
            }
        } else if (is_video_topic(m.topic)) {
            DecodedCompressedVideo v;
            if (decode_compressed_video(m.data, v) && !v.data.empty()) {
                auto& info = info_[m.topic];
                if (info.frames++ == 0) info.first_us = m.timestamp_us;
                info.last_us = m.timestamp_us;
                info.bytes += v.data.size();
                if (info.codec.empty()) info.codec = v.format;
                if (packet_is_keyframe(v.format, v.data.data(), v.data.size()))
                    keyframes_[m.topic].push_back(m.timestamp_us);
                auto dit = decoders_.find(m.topic);
                if (dit != decoders_.end() && dit->second) {
                    auto ex = extract_param_sets(v.format, v.data.data(), v.data.size());
                    if (!ex.empty()) dit->second->set_extradata(ex.data(), (int)ex.size());
                }
            }
        }
        return true;
    });
    for (auto& [_, kfs] : keyframes_) std::sort(kfs.begin(), kfs.end());

    // Learn each camera's coded size now (decode its first keyframe once) so the
    // panel layout is stable from the first frame instead of snapping later.
    for (auto& [topic, kfs] : keyframes_) {
        if (kfs.empty()) continue;
        auto dit = decoders_.find(topic);
        if (dit == decoders_.end() || !dit->second) continue;
        uint64_t kf = kfs.front();
        reader_.read_messages(kf, kf + 1, [&](const McapMessage& m) -> bool {
            if (m.topic != topic) return true;
            DecodedCompressedVideo v;
            if (!decode_compressed_video(m.data, v)) return true;
            if (auto f = dit->second->decode(v.format, v.data.data(), (int)v.data.size())) {
                info_[topic].width = f->width;
                info_[topic].height = f->height;
            }
            return false;
        });
        dit->second->flush(); // leave the decoder clean for playback
    }
}

VideoChannelInfo McapRecording::video_info(const std::string& ch) const {
    VideoChannelInfo out;
    auto it = info_.find(ch);
    if (it == info_.end()) return out;
    const StreamInfo& info = it->second;
    out.valid = true;
    out.width = info.width;
    out.height = info.height;
    out.codec = info.codec;
    out.frame_count = info.frames;
    if (info.last_us > info.first_us && info.frames > 1) {
        double span_s = (info.last_us - info.first_us) / 1e6;
        out.fps = (info.frames - 1) / span_s;
        out.bitrate_bps = info.bytes * 8.0 / span_s;
    }
    return out;
}

const std::vector<ScalarSample>& McapRecording::scalar_history(const std::string& ch) const {
    auto it = scalar_hist_.find(ch);
    return it == scalar_hist_.end() ? empty_scalar_ : it->second;
}

bool McapRecording::seek_video(uint64_t target_us, const std::function<bool()>& cancelled,
                               std::map<std::string, VideoFramePtr>& out) {
    if (cancelled()) return false;

    // Plan each camera independently (carried over from do_seek_catchup):
    //  - no keyframe at or before the target → target is inside this camera's
    //    undecodable opening GOP; blank it (out[ch] == nullptr);
    //  - decoder already sits past the target's keyframe and before the target
    //    → decode forward from there (the drag-right / normal-playback case);
    //  - otherwise flush and replay from the keyframe at or before the target.
    // The flush is deferred to the decode phase so a superseded seek touches
    // nothing.
    struct Plan { uint64_t start_us; bool flush; };
    std::map<std::string, Plan> plan;
    uint64_t read_start = target_us;
    for (auto& [topic, dec] : decoders_) {
        auto kit = keyframes_.find(topic);
        bool have_kf = kit != keyframes_.end() && !kit->second.empty() &&
                       kit->second.front() <= target_us;
        if (!have_kf) {
            dec->flush();
            decoder_pos_us_[topic] = 0;
            out[topic] = nullptr; // blank
            continue;
        }
        uint64_t kf = *(std::upper_bound(kit->second.begin(), kit->second.end(), target_us) - 1);
        uint64_t cur = decoder_pos_us_[topic];
        Plan tr;
        if (cur > 0 && cur >= kf && cur < target_us)
            tr = {cur + 1, false};
        else
            tr = {kf, true};
        plan[topic] = tr;
        read_start = std::min(read_start, tr.start_us);
    }

    // Read the window once; bucket each planned channel's packets in log order.
    // Mutates no decoder state, so an abort here is a clean no-op.
    std::map<std::string, std::vector<DecodedCompressedVideo>> packets;
    reader_.read_messages(read_start, target_us + 1, [&](const McapMessage& m) -> bool {
        if (cancelled()) return false;
        if (!is_video_topic(m.topic)) return true;
        auto it = plan.find(m.topic);
        if (it == plan.end() || m.timestamp_us < it->second.start_us) return true;
        if (decoders_.find(m.topic) == decoders_.end()) return true;
        DecodedCompressedVideo v;
        if (!decode_compressed_video(m.data, v)) return true;
        packets[m.topic].push_back(std::move(v));
        return true;
    });
    if (cancelled()) return false;

    // Replay each channel's chain: (optionally flush, then) decode_discard all
    // but the last packet, decode the last one at full quality. Independent
    // decoders → run channels on separate threads when at least one has a GOP.
    struct Out { VideoFramePtr frame; };
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
        uint64_t& pos = decoder_pos_us_.find(topic)->second; // distinct key per thread
        auto pit = plan.find(topic);
        if (pit != plan.end() && pit->second.flush) { dec->flush(); pos = 0; }
        dec->set_fast_replay(true);
        for (size_t i = 0; i + 1 < pkts.size(); ++i)
            dec->decode_discard(pkts[i].format, pkts[i].data.data(), (int)pkts[i].data.size());
        dec->set_fast_replay(false);
        DecodedCompressedVideo& v = pkts.back();
        Out o;
        auto frame = dec->decode(v.format, v.data.data(), (int)v.data.size());
        if (frame) {
            pos = v.timestamp_us; // decoder consumed up to here AND we paint it
            frame->timestamp_us = v.timestamp_us;
            o.frame = std::move(frame);
        } else {
            pos = 0; // decode failed: refs dropped internally, force keyframe replay next time
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

    // Always hand back a completed batch (the caller decides whether to paint) —
    // this keeps decoder position == painted frame, the invariant the
    // incremental-vs-flush plan above relies on.
    for (auto& [topic, o] : outs)
        if (o.frame) out[topic] = o.frame;
    return true;
}

} // namespace mp
