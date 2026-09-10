// LeRobot dataset (codebase_version v3.0) implementation of Recording.
// A dataset is a directory of parquet (per-frame state/action) + mp4 (camera
// video), many episodes packed into shared files. Each episode is one
// playable segment.
#pragma once

#include "recording.h"

#include "image_column_source.h"
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
    enum class VSrc { Mp4, Image }; // per video channel: mp4 file vs PNG-in-parquet

    struct Episode {
        int ep_index = 0;                    // episode_index — the reliable key
        int length = 0;                      // frame count (from the `length` column)
        int64_t from_index = 0, to_index = 0; // global row range — unreliable in some datasets
        int data_chunk = 0, data_file = 0;
        std::map<std::string, int> vid_chunk, vid_file;
        std::map<std::string, double> vid_from, vid_to; // seconds (mp4 slice)
        std::string task;
    };

    std::string data_file_path(const Episode& e) const; // resolves a real path
    void build_episode_file_map();                       // episode_index -> data parquet

    std::string dir_;
    std::map<int, std::string> ep_file_;
    double fps_ = 30.0;
    std::vector<std::string> video_keys_, scalar_keys_;
    std::map<std::string, VideoChannelInfo> vinfo_;
    std::map<std::string, ScalarChannelInfo> sinfo_;
    std::map<std::string, VSrc> vsrc_kind_;
    bool episodes_have_video_slices_ = false;
    std::vector<Episode> episodes_;

    int cur_ep_ = -1;
    uint64_t seg_len_us_ = 0;
    ParquetDB scalar_db_; // reused across episode switches (no open/close churn)
    std::map<std::string, std::vector<ScalarSample>> scalar_hist_;
    std::map<std::string, std::unique_ptr<Mp4Source>> mp4_;
    std::map<std::string, std::unique_ptr<ImageColumnSource>> img_;
    std::vector<ScalarSample> empty_scalar_;
    std::vector<AudioPoint> no_audio_;
};

} // namespace mp
