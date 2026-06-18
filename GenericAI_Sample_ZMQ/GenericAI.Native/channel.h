#pragma once

#include "pch.h"
#include "detector_motion.h"
#include "gai_abi.h"
#include "param_snapshot.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace gai {

// Process-wide ZMQ frame mode. When on, channels receive decoded frames via
// Channel::SubmitDecodedFrame (from the ZmqFrameReceiver) instead of polling the
// MMF. Set by GAI_StartZmqReceiver before the first /SetParameters arrives.
void SetZmqFrameMode(bool on);
bool IsZmqFrameMode();

// Per-channel worker. One thread per channel polls MMF "ChannelFrame_<port>"
// (written by spark.recorder), runs motion detection on new frames and fires
// the registered callback synchronously — a slow callback therefore only
// stalls its own channel. This replaces the production wrapper's
// MmfReader + SharedDetectorScheduler + ChannelPipeline stack; the MMF layout
// and the callback contract are identical.
class Channel {
public:
    explicit Channel(int port);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    void Start();
    void RequestStop();
    void Join();

    int Port() const { return port_; }

    // Called from the HTTP listener thread (via GAI_SetChannelParameters);
    // never blocks behind an in-flight detection pass.
    void ApplyParameters(const GAI_Settings& s);

    // ZMQ frame plane: hand one decoded I420 frame to this channel (called from
    // the single ZmqFrameReceiver thread). Non-blocking + drop-old: it copies the
    // frame into a one-slot buffer and wakes this channel's worker, which runs
    // detection on its own thread (so a slow callback only stalls its own channel,
    // matching the MMF path). Frames before the first /SetParameters are dropped.
    void SubmitDecodedFrame(const unsigned char* yuv_i420, int width, int height,
                            int size, std::uint64_t timestamp);

private:
    void Run();
    void RunMmf();   // MMF frame source (legacy default)
    void RunZmq();   // ZMQ frame source (waits on SubmitDecodedFrame)
    bool EnsureMapped();
    void Unmap();
    void RunDetection(const unsigned char* frame, int width, int height,
                      int size, std::uint64_t timestamp);

    int port_ = 0;

    ParamSnapshot params_;
    std::atomic<bool> has_params_{false};

    MotionDetector detector_;
    MotionDetectorContext detector_ctx_;

    HANDLE map_handle_ = INVALID_HANDLE_VALUE;
    void* mapped_view_ = nullptr;

    std::vector<unsigned char> frame_;   // reused copy buffer
    int fallback_counter_ = 0;
    int warned_size_ = 0;

    // ZMQ frame plane one-slot mailbox (producer: ZmqFrameReceiver thread;
    // consumer: this channel's RunZmq worker). zmq_pending_ holds the latest
    // decoded I420; a new frame overwrites an unconsumed one (drop-old).
    std::mutex zmq_mtx_;
    std::condition_variable zmq_cv_;
    std::vector<unsigned char> zmq_pending_;
    int zmq_w_ = 0;
    int zmq_h_ = 0;
    int zmq_size_ = 0;
    std::uint64_t zmq_ts_ = 0;
    bool zmq_has_frame_ = false;

    std::atomic<bool> running_{false};
    std::thread thread_;
};

// Registered via GAI_RegisterCallback; shared by all channels.
extern std::atomic<GAI_DetectionCallback> g_callback;

}  // namespace gai
