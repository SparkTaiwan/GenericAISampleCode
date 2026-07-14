#pragma once
#ifdef USE_ZMQ  // ZMQ frame transport (ffmpeg/libzmq) -- excluded from Release-NoZmq

#include <functional>
#include <memory>
#include <string>

namespace gai {

class Channel;

// Single process-wide receiver for the ZMQ frame plane. Connects a PULL socket to
// the recorder/AIServiceModule's bound PUSH endpoint, receives multiplexed frames
// (ZmqFrameHeader + NAL), demuxes by channel_id to the matching Channel, decodes
// the NAL to I420 (one NalDecoder per channel) and submits it via
// Channel::SubmitDecodedFrame.
//
// Connection model: the module BINDS, this side CONNECTS (so ZMQ auto-reconnects
// when the module / this process restarts). One PULL socket carries every channel.
//
// libzmq is confined to the .cpp via the Impl pimpl so includers stay free of the
// ZMQ dependency. The Sample has no scheduler, so the receiver is given a plain
// port->Channel lookup and the channel count (used for HWM / I/O-thread sizing).
class ZmqFrameReceiver {
public:
    using FindChannel = std::function<Channel*(int port)>;

    ZmqFrameReceiver(std::string endpoint, FindChannel find_channel, int channel_count);
    ~ZmqFrameReceiver();

    ZmqFrameReceiver(const ZmqFrameReceiver&) = delete;
    ZmqFrameReceiver& operator=(const ZmqFrameReceiver&) = delete;

    bool Start();  // connect + spawn the receive thread
    void Stop();   // stop the thread and close the socket (idempotent)

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace gai
#endif // USE_ZMQ
