#pragma once

#include <memory>
#include <string>

namespace gai {

class SharedDetectorScheduler;

// Single process-wide receiver for the ZMQ frame plane. Connects a PULL socket to
// the recorder/AIServiceModule's bound PUSH endpoint, receives multiplexed frames
// (ZmqFrameHeader + NAL), demuxes by channel_id to the matching ChannelPipeline,
// decodes the NAL to I420 (one NalDecoder per channel) and submits it.
//
// Connection model: the module BINDS, this side CONNECTS (so ZMQ auto-reconnects
// when the module / this process restarts). One PULL socket carries every channel.
//
// zmq.hpp / libzmq is confined to the .cpp via the Impl pimpl so includers stay
// free of the ZMQ dependency.
class ZmqFrameReceiver {
public:
    // endpoint e.g. "tcp://127.0.0.1:5556". scheduler must outlive this receiver
    // and is used to resolve channel_id -> ChannelPipeline.
    ZmqFrameReceiver(std::string endpoint, SharedDetectorScheduler* scheduler);
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
