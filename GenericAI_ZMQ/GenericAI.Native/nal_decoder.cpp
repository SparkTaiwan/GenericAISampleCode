#include "pch.h"
#ifdef USE_ZMQ  // ZMQ frame transport (ffmpeg/libzmq) -- excluded from Release-NoZmq
#include "nal_decoder.h"
#include "zmq_frame_header.h"
#include "host_log.h"

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace gai {

NalDecoder::NalDecoder(int codec) {
    AVCodecID id = (codec == static_cast<int>(ZmqCodec::H265)) ? AV_CODEC_ID_HEVC
                                                               : AV_CODEC_ID_H264;
    const AVCodec* dec = avcodec_find_decoder(id);
    if (!dec) {
        HostLog(HostLogLevel::Error, "NalDecoder: decoder not found for codec id " +
                std::to_string(static_cast<int>(id)));
        return;
    }
    ctx_ = avcodec_alloc_context3(dec);
    if (!ctx_) return;
    // Low-latency: emit frames as soon as they decode (surveillance, not B-heavy).
    ctx_->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx_->thread_count = 1;
    if (avcodec_open2(ctx_, dec, nullptr) < 0) {
        avcodec_free_context(&ctx_);
        ctx_ = nullptr;
        HostLog(HostLogLevel::Error, "NalDecoder: avcodec_open2 failed");
        return;
    }
    frame_ = av_frame_alloc();
    pkt_   = av_packet_alloc();
}

NalDecoder::~NalDecoder() {
    if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
    if (conv_data_[0]) { av_freep(&conv_data_[0]); }
    if (pkt_)   av_packet_free(&pkt_);
    if (frame_) av_frame_free(&frame_);
    if (ctx_)   avcodec_free_context(&ctx_);
}

void NalDecoder::Decode(const unsigned char* nal, int len, const FrameCb& on_frame) {
    if (!ctx_ || !nal || len <= 0) return;

    // av_packet data must be non-const; libavcodec does not modify it for decode.
    pkt_->data = const_cast<unsigned char*>(nal);
    pkt_->size = len;

    int ret = avcodec_send_packet(ctx_, pkt_);
    if (ret < 0) {
        // Common before the first IDR (missing reference / SPS) — not fatal, the
        // decoder recovers on the next keyframe.
        return;
    }
    while (true) {
        ret = avcodec_receive_frame(ctx_, frame_);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
        if (ret < 0) break;
        EmitPackedI420(on_frame);
        av_frame_unref(frame_);
    }
}

void NalDecoder::EmitPackedI420(const FrameCb& on_frame) {
    const int w = frame_->width;
    const int h = frame_->height;
    if (w <= 0 || h <= 0) return;

    const int packed_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, w, h, 1);
    if (packed_size <= 0) return;
    if (packed_.size() < static_cast<std::size_t>(packed_size))
        packed_.resize(static_cast<std::size_t>(packed_size));

    if (frame_->format == AV_PIX_FMT_YUV420P) {
        // Decoder already produced planar I420: copy straight to a packed buffer
        // (respecting linesize), no scaling.
        av_image_copy_to_buffer(packed_.data(), packed_size,
                                frame_->data, frame_->linesize,
                                AV_PIX_FMT_YUV420P, w, h, 1);
    } else {
        // Other pixel format (e.g. NV12 from a HW decoder): convert via swscale.
        if (!sws_ || sws_w_ != w || sws_h_ != h || sws_fmt_ != frame_->format) {
            if (sws_) { sws_freeContext(sws_); sws_ = nullptr; }
            sws_ = sws_getContext(w, h, static_cast<AVPixelFormat>(frame_->format),
                                  w, h, AV_PIX_FMT_YUV420P,
                                  SWS_BILINEAR, nullptr, nullptr, nullptr);
            sws_w_ = w; sws_h_ = h; sws_fmt_ = frame_->format;
            if (conv_data_[0]) av_freep(&conv_data_[0]);
            if (av_image_alloc(conv_data_, conv_linesize_, w, h, AV_PIX_FMT_YUV420P, 1) < 0) {
                conv_data_[0] = nullptr;
                return;
            }
            conv_w_ = w; conv_h_ = h;
        }
        if (!sws_ || !conv_data_[0]) return;
        sws_scale(sws_, frame_->data, frame_->linesize, 0, h, conv_data_, conv_linesize_);
        av_image_copy_to_buffer(packed_.data(), packed_size,
                                conv_data_, conv_linesize_,
                                AV_PIX_FMT_YUV420P, w, h, 1);
    }

    on_frame(packed_.data(), w, h);
}

}  // namespace gai
#endif // USE_ZMQ
