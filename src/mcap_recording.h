// MCAP implementation of Recording. Reads EgoViewer-style .mcap recordings
// (foxglove.CompressedVideo on /camera/*, ego.ImuSample on /imu/*,
// foxglove.RawAudio on /audio) — the H.264/H.265 keyframe index, GOP replay
// and per-camera parallel seek all live here.
#pragma once

#include "recording.h"

#include "mcap_reader.h"
#include "video_decoder.h"

#include <memory>
#include <mutex>

namespace mp {

class McapRecording : public Recording {
public:
    ~McapRecording() override;

    // Opens the file and preloads scalar history + the video keyframe index.
    // Called by open_recording(); returns false on failure.
    bool open(const std::string& path);

    uint64_t start_time_us() const override { return reader_.start_time_us(); }
    uint64_t end_time_us() const override { return reader_.end_time_us(); }
    const std::vector<std::string>& video_channels() const override { return video_topics_; }
    const std::vector<std::string>& scalar_channels() const override { return scalar_topics_; }
    bool has_audio() const override { return has_audio_; }
    VideoChannelInfo video_info(const std::string& ch) const override;
    const std::vector<ScalarSample>& scalar_history(const std::string& ch) const override;
    const std::vector<AudioPoint>& audio_history() const override { return audio_hist_; }
    bool seek_video(uint64_t target_us, const std::function<bool()>& cancelled,
                    std::map<std::string, VideoFramePtr>& out) override;

private:
    void preload();

    struct StreamInfo {
        uint64_t frames = 0, bytes = 0, first_us = 0, last_us = 0;
        int width = 0, height = 0; // coded size, from decoding the first keyframe
        std::string codec;
    };

    McapReader reader_;
    std::vector<std::string> topics_, video_topics_, scalar_topics_;
    std::map<std::string, std::unique_ptr<VideoDecoder>> decoders_;
    // Per video channel: sorted keyframe log times; the log time the decoder has
    // most recently consumed; stream totals.
    std::map<std::string, std::vector<uint64_t>> keyframes_;
    std::map<std::string, uint64_t> decoder_pos_us_;
    std::map<std::string, StreamInfo> info_;
    std::map<std::string, std::vector<ScalarSample>> scalar_hist_;
    std::vector<AudioPoint> audio_hist_;
    std::vector<ScalarSample> empty_scalar_;
    bool has_audio_ = false;
};

} // namespace mp
