#pragma once

#include "pch.h"
#include "detector_motion.h"
#include "gai_abi.h"
#include "param_snapshot.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

namespace gai {

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

private:
    void Run();
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

    std::atomic<bool> running_{false};
    std::thread thread_;
};

// Registered via GAI_RegisterCallback; shared by all channels.
extern std::atomic<GAI_DetectionCallback> g_callback;

}  // namespace gai
