// Qt-free port of EgoViewer's src/data/FfmpegVideoDecoder.{h,cpp}. H.264/
// H.265 packet decoder — one instance per video stream. The seek-time
// "decode a chain of frames but only convert the last one" optimisation
// (decode_discard) is carried over verbatim.
#pragma once

#include "video_frame.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVCodec;
struct AVFrame;

namespace mp {

class VideoDecoder {
public:
    VideoDecoder();
    ~VideoDecoder();

    // `codec` is "h264" / "h265" / "hevc" (from the CompressedVideo message).
    VideoFramePtr decode(const std::string& codec, const uint8_t* data, int size);
    // Advances the decoder's reference-frame state through this packet but
    // skips the (allocation + full copy) conversion to a VideoFrame.
    void decode_discard(const std::string& codec, const uint8_t* data, int size);

    void reset();
    void flush(); // resync at next keyframe, keeping parsed SPS/PPS

    // While replaying a GOP toward a seek target, skip the in-loop deblocking
    // filter and any non-reference frames — the intermediate frames only need
    // to be good enough to serve as references. Turn back off before decoding
    // the frame that will actually be shown. Safe to call any time.
    void set_fast_replay(bool on);

    // Out-of-band parameter sets (Annex-B SPS/PPS bytes). When set, every
    // keyframe decodes standalone — needed for streams whose keyframes don't
    // carry inline SPS/PPS (so seeks land without "non-existing PPS" errors).
    // Set before the first decode().
    void set_extradata(const uint8_t* data, int size);

private:
    bool ensure_codec(const std::string& codec);
    void apply_replay_flags(); // push fast_replay_ into ctx_; call with mutex_ held
    VideoFramePtr frame_to_buffer(const AVFrame* frame) const;

    std::mutex mutex_;
    int codec_id_ = 0; // AVCodecID
    const AVCodec* codec_ = nullptr;
    AVCodecContext* ctx_ = nullptr;
    int decode_error_count_ = 0;
    bool fast_replay_ = false;
    std::vector<uint8_t> extradata_;
};

} // namespace mp
