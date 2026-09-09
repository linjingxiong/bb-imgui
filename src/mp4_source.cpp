#include "mp4_source.h"

#include "video_decoder.h" // av_frame_to_video_frame

#include <algorithm>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>
}

namespace mp {

struct Mp4Source::FF {
    AVFormatContext* fmt = nullptr;
    AVCodecContext* ctx = nullptr;
    AVPacket* pkt = nullptr;
    AVFrame* frame = nullptr;
    int vs = -1; // video stream index
    ~FF() {
        if (frame) av_frame_free(&frame);
        if (pkt) av_packet_free(&pkt);
        if (ctx) avcodec_free_context(&ctx);
        if (fmt) avformat_close_input(&fmt);
    }
};

Mp4Source::Mp4Source() = default;
Mp4Source::~Mp4Source() { close(); }
void Mp4Source::close() { ff_.reset(); }

bool Mp4Source::open(const std::string& path, uint64_t from_us, uint64_t to_us) {
    close();
    auto ff = std::make_unique<FF>();

    if (avformat_open_input(&ff->fmt, path.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "mp4: open failed: %s\n", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(ff->fmt, nullptr) < 0) return false;

    ff->vs = av_find_best_stream(ff->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (ff->vs < 0) { std::fprintf(stderr, "mp4: no video stream\n"); return false; }
    AVStream* st = ff->fmt->streams[ff->vs];

    const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec) {
        std::fprintf(stderr, "mp4: no decoder for codec id %d\n", (int)st->codecpar->codec_id);
        return false;
    }
    ff->ctx = avcodec_alloc_context3(dec);
    if (!ff->ctx || avcodec_parameters_to_context(ff->ctx, st->codecpar) < 0) return false;
    ff->ctx->thread_count = 0;
    ff->ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
    if (avcodec_open2(ff->ctx, dec, nullptr) < 0) {
        std::fprintf(stderr, "mp4: avcodec_open2 failed (%s)\n", dec->name);
        return false;
    }

    ff->pkt = av_packet_alloc();
    ff->frame = av_frame_alloc();

    width_ = ff->ctx->width ? ff->ctx->width : st->codecpar->width;
    height_ = ff->ctx->height ? ff->ctx->height : st->codecpar->height;
    codec_name_ = avcodec_get_name(st->codecpar->codec_id);

    const int64_t start = st->start_time == AV_NOPTS_VALUE ? 0 : st->start_time;
    const AVRational us = {1, 1'000'000};
    from_us_ = from_us;
    win_from_pts_ = start + av_rescale_q((int64_t)from_us, us, st->time_base);
    if (to_us == 0) {
        win_to_pts_ = INT64_MAX;
        int64_t dur = st->duration == AV_NOPTS_VALUE ? 0 : st->duration;
        win_len_us_ = dur ? (uint64_t)av_rescale_q(dur, st->time_base, us) - from_us : 0;
    } else {
        win_to_pts_ = start + av_rescale_q((int64_t)to_us, us, st->time_base);
        win_len_us_ = to_us > from_us ? to_us - from_us : 0;
    }
    cursor_pts_ = INT64_MIN;
    ff_ = std::move(ff);
    return true;
}

VideoFramePtr Mp4Source::frame_at(uint64_t target_us, const std::function<bool()>& cancelled) {
    if (!ff_) return nullptr;
    AVStream* st = ff_->fmt->streams[ff_->vs];
    const AVRational us = {1, 1'000'000};
    const int64_t tgt_pts = win_from_pts_ + av_rescale_q((int64_t)target_us, us, st->time_base);

    // Decide: continue forward from the cursor, or seek to the keyframe before
    // the target. Seek when going backward or jumping more than ~1.5s ahead.
    const int64_t fwd_budget = av_rescale_q(1'500'000, us, st->time_base);
    bool need_seek = cursor_pts_ == INT64_MIN || tgt_pts < cursor_pts_ ||
                     tgt_pts - cursor_pts_ > fwd_budget;
    if (need_seek) {
        if (av_seek_frame(ff_->fmt, ff_->vs, tgt_pts, AVSEEK_FLAG_BACKWARD) < 0) {
            // fall back to a rewind to the window start
            av_seek_frame(ff_->fmt, ff_->vs, win_from_pts_, AVSEEK_FLAG_BACKWARD);
        }
        avcodec_flush_buffers(ff_->ctx);
        cursor_pts_ = INT64_MIN;
    }

    VideoFramePtr best;
    int64_t best_pts = INT64_MIN;
    bool eof = false;

    auto take = [&](AVFrame* f) {
        int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
        if (pts == AV_NOPTS_VALUE) return;
        cursor_pts_ = pts;
        if (pts > tgt_pts) { eof = true; return; } // overshot — stop keeping
        if (pts < win_from_pts_) return;           // before the episode window
        if (auto vf = av_frame_to_video_frame(f)) {
            int64_t rel_us = av_rescale_q(pts - win_from_pts_, st->time_base, us);
            vf->timestamp_us = (uint64_t)std::max<int64_t>(0, rel_us);
            best = std::move(vf);
            best_pts = pts;
        }
    };

    while (!eof) {
        if (cancelled()) return nullptr;
        int r = av_read_frame(ff_->fmt, ff_->pkt);
        if (r < 0) { // EOF: flush the decoder
            avcodec_send_packet(ff_->ctx, nullptr);
            while (avcodec_receive_frame(ff_->ctx, ff_->frame) == 0) {
                take(ff_->frame);
                av_frame_unref(ff_->frame);
                if (eof) break;
            }
            break;
        }
        if (ff_->pkt->stream_index != ff_->vs) { av_packet_unref(ff_->pkt); continue; }
        if (win_to_pts_ != INT64_MAX && ff_->pkt->pts != AV_NOPTS_VALUE &&
            ff_->pkt->pts >= win_to_pts_ && best) {
            av_packet_unref(ff_->pkt);
            break; // past the episode's slice
        }
        avcodec_send_packet(ff_->ctx, ff_->pkt);
        av_packet_unref(ff_->pkt);
        while (avcodec_receive_frame(ff_->ctx, ff_->frame) == 0) {
            take(ff_->frame);
            av_frame_unref(ff_->frame);
            if (eof) break;
        }
    }
    (void)best_pts;
    return best;
}

} // namespace mp
