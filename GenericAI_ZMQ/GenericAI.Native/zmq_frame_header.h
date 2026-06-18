#pragma once

#include <cstdint>

namespace gai {

// Wire header that prefixes every ZMQ frame message (recorder/AIServiceModule ->
// this wrapper). Sent as the FIRST part of a 2-part ZMQ message; the SECOND part
// is the raw encoded NAL bytes for one coded picture (access unit).
//
//   part 0: ZmqFrameHeader (this struct, packed)
//   part 1: NAL bytes      (length == payload_sz)
//
// channel_id multiplexes many channels over a single socket: the receiver routes
// each frame to the ChannelPipeline whose Port() == channel_id. No per-channel
// socket / port is needed.
#pragma pack(push, 1)
struct ZmqFrameHeader {
    uint32_t magic;        // kZmqFrameMagic
    uint16_t version;      // kZmqFrameVersion
    uint8_t  codec;        // ZmqCodec (1=H264, 2=H265)
    uint8_t  is_keyframe;  // 1 = IDR/keyframe, 0 = P/B frame
    uint16_t width;        // coded width  (informational; decoder is authoritative)
    uint16_t height;       // coded height
    uint32_t channel_id;   // routes to ChannelPipeline::Port()
    uint64_t timestamp;    // Windows FILETIME (100ns ticks, UTC)
    uint32_t payload_sz;   // NAL byte count in part 1
};
#pragma pack(pop)

constexpr uint32_t kZmqFrameMagic   = 0x5A4D4600u;  // 'Z''M''F''\0'
constexpr uint16_t kZmqFrameVersion = 1;

enum class ZmqCodec : uint8_t {
    H264 = 1,
    H265 = 2,
};

}  // namespace gai
