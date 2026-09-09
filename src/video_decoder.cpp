#include "video_decoder.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
}

namespace mp {

namespace {
AVCodecID codec_id_for(const std::string& c) {
    if (c == "h264" || c == "avc1") return AV_CODEC_ID_H264;
    if (c == "h265" || c == "hevc" || c == "hev1") return AV_CODEC_ID_HEVC;
    return AV_CODEC_ID_NONE;
}
} // namespace

VideoDecoder::VideoDecoder() { av_log_set_level(AV_LOG_ERROR); }
VideoDecoder::~VideoDecoder() { reset(); }

void VideoDecoder::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) avcodec_free_context(&ctx_);
    codec_ = nullptr;
    codec_id_ = AV_CODEC_ID_NONE;
    decode_error_count_ = 0;
}

void VideoDecoder::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ctx_) avcodec_flush_buffers(ctx_);
    decode_error_count_ = 0;
}

void VideoDecoder::apply_replay_flags() {
    if (!ctx_) return;
    ctx_->skip_loop_filter = fast_replay_ ? AVDISCARD_ALL : AVDISCARD_DEFAULT;
    ctx_->skip_frame = fast_replay_ ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
}

void VideoDecoder::set_fast_replay(bool on) {
    std::lock_guard<std::mutex> lock(mutex_);
    fast_replay_ = on;
    apply_replay_flags();
}

void VideoDecoder::set_extradata(const uint8_t* data, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data || size <= 0) return;
    std::vector<uint8_t> next(data, data + size);
    if (next == extradata_) return;
    extradata_ = std::move(next);
    // Re-create the context on next decode so avcodec_open2 sees the new params.
    if (ctx_) avcodec_free_context(&ctx_);
    codec_ = nullptr;
    codec_id_ = 0;
}

bool VideoDecoder::ensure_codec(const std::string& codec) {
    AVCodecID want = codec_id_for(codec);
    if (want == AV_CODEC_ID_NONE) return false;
    if (ctx_ && codec_id_ == want) return true;
    if (ctx_) avcodec_free_context(&ctx_);
    codec_id_ = AV_CODEC_ID_NONE;
    codec_ = avcodec_find_decoder(want);
    if (!codec_) return false;
    ctx_ = avcodec_alloc_context3(codec_);
    if (!ctx_) { codec_ = nullptr; return false; }
    // Slice threading only: it parallelises a single frame's decode across
    // cores with no frame-reorder delay, so the "feed chunks, take the last
    // frame" model is unchanged. (Frame threading would be faster still but
    // buffers N frames before emitting, which breaks that model.)
    ctx_->thread_count = 0; // auto — one per logical CPU
    ctx_->thread_type = FF_THREAD_SLICE;
    ctx_->flags2 |= AV_CODEC_FLAG2_FAST;
    apply_replay_flags();
    if (!extradata_.empty()) {
        ctx_->extradata =
            (uint8_t*)av_mallocz(extradata_.size() + AV_INPUT_BUFFER_PADDING_SIZE);
        if (ctx_->extradata) {
            std::memcpy(ctx_->extradata, extradata_.data(), extradata_.size());
            ctx_->extradata_size = (int)extradata_.size();
        }
    }
    if (avcodec_open2(ctx_, codec_, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        codec_ = nullptr;
        return false;
    }
    codec_id_ = want;
    return true;
}

VideoFramePtr VideoDecoder::frame_to_buffer(const AVFrame* frame) const {
    return av_frame_to_video_frame(frame);
}

VideoFramePtr av_frame_to_video_frame(const AVFrame* frame) {
    if (!frame || frame->width <= 0 || frame->height <= 0) return {};

    auto copy_plane = [](const uint8_t* src, int src_stride, int w, int h,
                         std::vector<uint8_t>& dst, int& dst_stride) {
        if (!src || w <= 0 || h <= 0) { dst_stride = 0; return; }
        dst_stride = w;
        dst.resize((size_t)w * h);
        for (int y = 0; y < h; ++y)
            std::memcpy(dst.data() + (size_t)y * w, src + (size_t)y * src_stride, w);
    };

    auto pix = static_cast<AVPixelFormat>(frame->format);
    auto out = std::make_shared<VideoFrame>();
    out->width = frame->width;
    out->height = frame->height;
    out->full_range = frame->color_range == AVCOL_RANGE_JPEG;
    out->bt601 = frame->colorspace == AVCOL_SPC_BT470BG || frame->colorspace == AVCOL_SPC_SMPTE170M;

    if (pix == AV_PIX_FMT_YUV420P || pix == AV_PIX_FMT_YUVJ420P) {
        out->format = VideoFrame::PixelFormat::Yuv420P;
        copy_plane(frame->data[0], frame->linesize[0], frame->width, frame->height,
                   out->planes[0], out->strides[0]);
        copy_plane(frame->data[1], frame->linesize[1], frame->width / 2, frame->height / 2,
                   out->planes[1], out->strides[1]);
        copy_plane(frame->data[2], frame->linesize[2], frame->width / 2, frame->height / 2,
                   out->planes[2], out->strides[2]);
        return out;
    }
    if (pix == AV_PIX_FMT_YUV422P || pix == AV_PIX_FMT_YUVJ422P) {
        // Downsample UV vertically 4:2:2 -> 4:2:0.
        out->format = VideoFrame::PixelFormat::Yuv420P;
        copy_plane(frame->data[0], frame->linesize[0], frame->width, frame->height,
                   out->planes[0], out->strides[0]);
        const int uvw = frame->width / 2, uvh = frame->height / 2;
        out->strides[1] = out->strides[2] = uvw;
        out->planes[1].resize((size_t)uvw * uvh);
        out->planes[2].resize((size_t)uvw * uvh);
        for (int y = 0; y < uvh; ++y) {
            std::memcpy(out->planes[1].data() + (size_t)y * uvw,
                        frame->data[1] + (size_t)(y * 2) * frame->linesize[1], uvw);
            std::memcpy(out->planes[2].data() + (size_t)y * uvw,
                        frame->data[2] + (size_t)(y * 2) * frame->linesize[2], uvw);
        }
        return out;
    }
    if (pix == AV_PIX_FMT_NV12) {
        out->format = VideoFrame::PixelFormat::Nv12;
        copy_plane(frame->data[0], frame->linesize[0], frame->width, frame->height,
                   out->planes[0], out->strides[0]);
        copy_plane(frame->data[1], frame->linesize[1], frame->width, frame->height / 2,
                   out->planes[1], out->strides[1]);
        return out;
    }
    return {};
}

VideoFramePtr VideoDecoder::decode(const std::string& codec, const uint8_t* data, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data || size <= 0 || !ensure_codec(codec)) return {};

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return {};
    }
    packet->data = const_cast<uint8_t*>(data);
    packet->size = size;

    VideoFramePtr output;
    int send_ret = avcodec_send_packet(ctx_, packet);
    if (send_ret == AVERROR(EAGAIN)) {
        while (avcodec_receive_frame(ctx_, frame) >= 0) {
            if (auto c = frame_to_buffer(frame)) output = c;
            av_frame_unref(frame);
        }
        send_ret = avcodec_send_packet(ctx_, packet);
    }
    if (send_ret >= 0) {
        while (true) {
            int ret = avcodec_receive_frame(ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { avcodec_flush_buffers(ctx_); ++decode_error_count_; break; }
            if (auto c = frame_to_buffer(frame)) output = c;
            av_frame_unref(frame);
        }
    } else if (send_ret != AVERROR(EAGAIN)) {
        avcodec_flush_buffers(ctx_);
        ++decode_error_count_;
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
    return output;
}

void VideoDecoder::decode_discard(const std::string& codec, const uint8_t* data, int size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!data || size <= 0 || !ensure_codec(codec)) return;

    AVPacket* packet = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    if (!packet || !frame) {
        av_packet_free(&packet);
        av_frame_free(&frame);
        return;
    }
    packet->data = const_cast<uint8_t*>(data);
    packet->size = size;

    int send_ret = avcodec_send_packet(ctx_, packet);
    if (send_ret == AVERROR(EAGAIN)) {
        while (avcodec_receive_frame(ctx_, frame) >= 0) av_frame_unref(frame);
        send_ret = avcodec_send_packet(ctx_, packet);
    }
    if (send_ret >= 0) {
        while (true) {
            int ret = avcodec_receive_frame(ctx_, frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) { avcodec_flush_buffers(ctx_); break; }
            av_frame_unref(frame);
        }
    } else if (send_ret != AVERROR(EAGAIN)) {
        avcodec_flush_buffers(ctx_);
    }

    av_packet_free(&packet);
    av_frame_free(&frame);
}

} // namespace mp
