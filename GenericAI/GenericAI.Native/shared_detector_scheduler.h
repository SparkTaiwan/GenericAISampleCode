#pragma once

#include "bounded_queue.h"
#include "channel_pipeline.h"
#include "idetector.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace gai {

// Owns the single IDetector and the only InferLoop thread in the process.
// Drives detection across N ChannelPipelines in strict round-robin (spec §12)
// with skip-empty: a channel is visited only if it is configured
// (HasParams()) and has pending work (PendingInferDepth() > 0). When the
// whole ring is empty the loop sleeps 1 ms (spec §10) so this is not a busy
// wait.
//
// Threading contract:
//   - channels_ / detector_: written only by Start() / Stop(). While
//     running_ == true they are read-only; no mutex.
//   - rr_cursor_: read and written only by InferLoop; no mutex.
//   - running_: atomic acquire/release; the only handshake between
//     InferLoop and Stop().
//   - FindByPort() / ChannelAt() / ChannelCount(): called from the C ABI
//     boundary (exports.cpp), which holds its own g_lifecycle_mtx around
//     the scheduler's Start/Stop. Those accessors take no lock themselves.
//
// Shutdown ordering (also see channel_pipeline.h ordering contract):
//   1. running_ = false
//   2. for each channel: channel->Stop()   (close queues, clear has_params_,
//      request MmfReader stop — does NOT invalidate any pointers)
//   3. infer_thread_.join()                (InferLoop has now definitely
//      exited and no longer touches channels)
//   4. for each channel: channel->Join()   (only now safe to reset
//      pool_/queues/ctx held by the channel)
//   5. detector_.reset()
class SharedDetectorScheduler {
public:
    SharedDetectorScheduler();
    ~SharedDetectorScheduler();

    SharedDetectorScheduler(const SharedDetectorScheduler&) = delete;
    SharedDetectorScheduler& operator=(const SharedDetectorScheduler&) = delete;

    // Takes ownership of the detector and the channels. Channels must already
    // have been Start()ed by the caller (so their queues / DispatchThread are
    // ready). Per-channel DetectorContext is created here via
    // ChannelPipeline::InitContext. Returns false on bad input or if any
    // CreateContext throws (e.g. PersonDetector with uninitialized model
    // dims); in that case the scheduler rolls back to the not-started state.
    bool Start(std::unique_ptr<IDetector> detector,
               std::vector<std::unique_ptr<ChannelPipeline>> channels);

    // Idempotent. Follows the shutdown ordering documented above.
    void Stop();

    // Called from C ABI lifecycle methods (Step 4 exports.cpp). Read-only over
    // channels_; safe while the InferLoop is running.
    ChannelPipeline* FindByPort(int port);
    ChannelPipeline* ChannelAt(std::size_t i);
    std::size_t      ChannelCount() const { return channels_.size(); }

    // Detector backend label ("CPU" / "DirectML(0)" / ""). Caller (exports.cpp)
    // serializes against Stop() with its own lifecycle mutex; no internal lock.
    std::string      Backend() const;

private:
    // Single-thread topology (Motion, or kEnablePipelinedInference == false).
    void InferLoop();
    // Two-thread topology (route B): PreLoop owns CPU preprocess + round-robin
    // pick; GpuLoop owns session.Run + CPU postprocess. Bridged by
    // pre_to_gpu_q_.
    void PreLoop();
    void GpuLoop();
    ChannelPipeline* PickNextChannel();

    // Queue item handed off between PreLoop and GpuLoop. detector_slot is
    // PersonDetector::Phase1Prepare's return value: >=0 (real slot), -1
    // (stride-skipped; GpuLoop CommitEmpty's the channel without touching
    // the detector pool). view is moved through the queue so Phase3 sees
    // the same ROI / params snapshot as Phase1 even after /SetParameters
    // races behind it.
    struct PreToGpuItem {
        ChannelPipeline* channel = nullptr;
        FrameSlot* slot = nullptr;
        ParamSnapshot::View view;
        int detector_slot = -1;
        std::uint64_t timestamp = 0;
    };
    // cap = 4 is intentionally tiny: the detector's own pool (K=2) is the
    // real backpressure, plus this queue is only crossed by one writer and
    // one reader.
    BoundedQueue<PreToGpuItem> pre_to_gpu_q_{4};

    std::unique_ptr<IDetector> detector_;
    std::vector<std::unique_ptr<ChannelPipeline>> channels_;
    std::size_t rr_cursor_ = 0;
    std::thread infer_thread_;
    std::thread pre_thread_;
    std::thread gpu_thread_;
    std::atomic<bool> running_{false};
    bool pipelined_ = false;   // locked at Start(), read by Stop()
};

}  // namespace gai
