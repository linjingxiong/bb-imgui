// LeRobot dataset (codebase_version v3.0) implementation of Recording.
// A dataset is a directory of parquet (per-frame state/action) + mp4 (camera
// video), many episodes packed into shared files. Each episode is one
// playable segment.
#pragma once

#include "recording.h"

#include "mp4_source.h"

#include <map>
#include <memory>

namespace mp {

class LeRobotRecording : public Recording {
public:
    ~LeRobotRecording() override;

    // `dir` must contain meta/info.json with a "v3.*" codebase_version.
    bool open(const std::string& dir);

    uint64_t start_time_us() const override { return 0; }
    uint64_t end_time_us() const override { return seg_len_us_; }
    const std::vector<std::string>& video_channels() const override { return video_keys_; }
    const std::vector<std::string>& scalar_channels() const override { return scalar_keys_; }
    bool has_audio() const override { return false; }
    VideoChannelInfo video_info(const std::string& ch) const override;
    ScalarChannelInfo scalar_info(const std::string& ch) const override;
    const std::vector<ScalarSample>& scalar_history(const std::string& ch) const override;
    const std::vector<AudioPoint>& audio_history() const override { return no_audio_; }
    bool seek_video(uint64_t target_us, const std::function<bool()>& cancelled,
                    std::map<std::string, VideoFramePtr>& out) override;

    int segment_count() const override { return (int)episodes_.size(); }
    SegmentInfo segment_info(int i) const override;
    int current_segment() const override { return cur_ep_; }
    bool select_segment(int i) override;

private:
    struct Episode {
        int64_t from_index = 0, to_index = 0; // global row range in the data parquet
        int data_chunk = 0, data_file = 0;
        std::map<std::string, int> vid_chunk, vid_file;
        std::map<std::string, double> vid_from, vid_to; // seconds
        std::string task;
    };

    std::string dir_;
    double fps_ = 30.0;
    std::vector<std::string> video_keys_, scalar_keys_;
    std::map<std::string, VideoChannelInfo> vinfo_;
    std::map<std::string, ScalarChannelInfo> sinfo_;
    std::vector<Episode> episodes_;

    int cur_ep_ = -1;
    uint64_t seg_len_us_ = 0;
    std::map<std::string, std::vector<ScalarSample>> scalar_hist_;
    std::map<std::string, std::unique_ptr<Mp4Source>> mp4_;
    std::vector<ScalarSample> empty_scalar_;
    std::vector<AudioPoint> no_audio_;
};

} // namespace mp
