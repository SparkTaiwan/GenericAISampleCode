#pragma once

#include "bounded_queue.h"
#include "buffer_pool.h"
#include "gai_abi.h"
#include "idetector.h"
#include "mmf_reader.h"
#include "param_snapshot.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace gai {

// Carries detection results plus the originating frame from the shared
// InferLoop (in SharedDetectorScheduler, Step 3) to this channel's
// DispatchThread. The frame slot is owned for the lifetime of the packet and
// returned to the pool by DispatchThread.
struct ChannelDetectionPacket {
    FrameSlot* frame = nullptr;
    std::vector<GAI_Roi> rois_flat;
    int rois_count = 0;
    int node_count = 0;
};

// Per-channel pipeline: holds the MMF reader, buffer pool, queues, params
// snapshot, dispatch thread, and the channel's per-detector context. The
// shared InferLoop (SharedDetectorScheduler, Step 3) drives detection by
// calling TryAcquireWork / CommitResult / CommitEmpty / CommitError; this
// class does not own an inference thread.
class ChannelPipeline {
public:
    explicit ChannelPipeline(int port);
    ~ChannelPipeline();

    ChannelPipeline(const ChannelPipeline&) = delete;
    ChannelPipeline& operator=(const ChannelPipeline&) = delete;

    // Build queues + dispatch thread. BufferPool / MmfReader are deferred to
    // the first ApplyParameters with valid resolution.
    void Start();
    // Signal stop; closes queues, asks the reader to stop, clears has_params_
    // so the scheduler skips this channel immediately. Non-blocking.
    //
    // ORDERING CONTRACT: the caller (SharedDetectorScheduler) MUST stop its
    // shared InferLoop BEFORE invoking Stop()/Join() on any ChannelPipeline.
    // Otherwise the scheduler may still be inside CommitResult/CommitEmpty/
    // CommitError on this channel while Join() resets pool_/dispatch_q_ —
    // use-after-free on shutdown. See docs/spec.md §11.3.
    void Stop();
    // Joins the reader + dispatch threads, then releases the pool / queues /
    // context. Call after Stop(). Same ordering contract as Stop().
    void Join();

    void ApplyParameters(const GAI_Settings& s);
    void SetCallback(GAI_DetectionCallback cb);

    int Port() const { return port_; }

    // Scheduler-facing. Stateless detector model: the scheduler reads
    // PendingInferDepth + HasParams to pick a channel, then TryAcquireWork to
    // grab a slot + a snapshot view; it commits the result via CommitResult /
    // CommitEmpty / CommitError. The channel's DetectorContext is supplied
    // via Context().
    bool   HasParams() const { return has_params_.load(std::memory_order_acquire); }
    std::size_t PendingInferDepth() const { return infer_q_ ? infer_q_->Size() : 0; }
    bool   TryAcquireWork(FrameSlot*& slot, ParamSnapshot::View& view);
    void   CommitResult(FrameSlot* slot, std::vector<GAI_Roi>&& flat,
                        int rois_count, int node_count);
    void   CommitEmpty(FrameSlot* slot);
    void   CommitError(FrameSlot* slot);

    DetectorContext* Context() { return detector_ctx_.get(); }
    // Called once by the scheduler after the shared detector is built, so the
    // channel can allocate its per-channel detector context.
    void InitContext(IDetector* shared_detector);

private:
    void DispatchLoop();

    int port_ = 0;

    std::unique_ptr<DetectorContext> detector_ctx_;
    ParamSnapshot params_;

    std::unique_ptr<BufferPool> pool_;
    std::unique_ptr<BoundedQueue<FrameSlot*>> infer_q_;
    std::unique_ptr<BoundedQueue<ChannelDetectionPacket>> dispatch_q_;
    std::unique_ptr<MmfReader> mmf_reader_;

    std::thread dispatch_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> has_params_{false};

    // BufferPool + MmfReader are built on the first SetParameters with valid
    // resolution. Guarded so concurrent ApplyParameters calls do not race;
    // has_params_ is the durable flag (read lock-free by the scheduler).
    std::mutex start_mtx_;

    std::mutex cb_mtx_;
    GAI_DetectionCallback callback_ = nullptr;
};

}  // namespace gai
