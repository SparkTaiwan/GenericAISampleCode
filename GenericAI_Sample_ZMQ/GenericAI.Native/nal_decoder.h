#pragma once

#include <cstdint>
#include <functional>
#include <vector>

// Forward declarations so this header has no FFmpeg dependency (the .cpp does).
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;

namespace gai {

// Per-channel H.264/H.265 decoder (FFmpeg/libavcodec). One coded picture (access
// unit) of Annex-B NAL bytes in -> zero or more tightly-packed I420 frames out.
//
// One instance per channel: an H.264 stream is stateful (SPS/PPS + reference
// frames), so channels must not share a decoder. SPS/PPS are expected in-band
// (sent before/with the first IDR); libavcodec picks them up automatically.
class NalDecoder {
public:
    // codec: ZmqCodec value (1 = H264, 2 = H265).
    explicit NalDecoder(int codec);
    ~NalDecoder();

    NalDecoder(const NalDecoder&) = delete;
    NalDecoder& operator=(const NalDecoder&) = delete;

    bool Valid() const { return ctx_ != nullptr; }

    // i420 points to width*height*3/2 packed bytes, valid only for the call.
    using FrameCb = std::function<void(const unsigned char* i420, int width, int height)>;

    // Feed one access unit. Calls on_frame for each decoded picture (usually one;
    // can be zero while the decoder waits for an IDR, or >1 with B-frame reorder).
    void Decode(const unsigned char* nal, int len, const FrameCb& on_frame);

private:
    void EmitPackedI420(const FrameCb& on_frame);

    AVCodecContext* ctx_   = nullptr;
    AVFrame*        frame_ = nullptr;
    AVPacket*       pkt_   = nullptr;

    // Lazy swscale path, only used when the decoder output is not already I420.
    SwsContext* sws_      = nullptr;
    int         sws_w_    = 0;
    int         sws_h_    = 0;
    int         sws_fmt_  = -1;
    unsigned char* conv_data_[4]     = { nullptr, nullptr, nullptr, nullptr };
    int            conv_linesize_[4] = { 0, 0, 0, 0 };
    int            conv_w_ = 0;
    int            conv_h_ = 0;

    std::vector<unsigned char> packed_;  // reused output buffer
};

}  // namespace gai
