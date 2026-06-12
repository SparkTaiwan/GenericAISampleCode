#include "pch.h"
#include "shared_detector_scheduler.h"
#include "gai_config.h"
#include "timing_recorder.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <utility>

namespace gai {

namespace {

// Maps the view's ROI rects and original polygon points from the
// /SetParameters reference space onto the actual frame resolution, in place.
// The view is this iteration's private copy (ParamSnapshot::Take), so the
// stored snapshot keeps the reference-space coordinates for later frames.
// Doing it here covers both detector modes and both routes at once: Motion
// scans the scaled rects and echoes the scaled polygons, Person compares its
// frame-space boxes against the scaled rects, and the pipelined route carries
// the scaled view through the queue into Phase3Post.
void ScaleViewToFrame(ParamSnapshot::View& view, int frame_w, int frame_h) {
    const int cfg_w = view.params.image_width;
    const int cfg_h = view.params.image_height;
    if (!RoiScaleNeeded(cfg_w, cfg_h, frame_w, frame_h)) return;
    for (auto& r : view.roi_rects) {
        ScaleRoiRectToFrame(r, cfg_w, cfg_h, frame_w, frame_h);
    }
    for (auto& poly : view.original_roi_points) {
        for (auto& pt : poly) {
            pt.x = ScaleCoordToFrame(pt.x, cfg_w, frame_w);
            pt.y = ScaleCoordToFrame(pt.y, cfg_h, frame_h);
        }
    }
}

}  // namespace

SharedDetectorScheduler::SharedDetectorScheduler() = default;

SharedDetectorScheduler::~SharedDetectorScheduler() {
    Stop();
}

bool SharedDetectorScheduler::Start(std::unique_ptr<IDetector> detector,
                                    std::vector<std::unique_ptr<ChannelPipeline>> channels) {
    // Validate before claiming running_ so a rejected Start never has to
    // un-flip the flag.
    if (!detector || channels.empty()) return false;
    if (running_.exchange(true)) return false;

    detector_ = std::move(detector);
    channels_ = std::move(channels);

    try {
        for (auto& c : channels_) {
            if (c) c->InitContext(detector_.get());
        }
    } catch (...) {
        // PersonDetector::CreateContext throws when model dims aren't ready
        // (detector_person.cpp:748). Roll back without letting a second
        // exception escape during unwind — ~ChannelPipeline joins threads
        // which themselves may throw.
        try { channels_.clear(); } catch (...) {}
        detector_.reset();
        running_.store(false);
        return false;
    }

    rr_cursor_ = 0;
    // Lock topology choice at Start time. Detector capability + compile-time
    // flag must both opt in; Motion (HasPipelined==false) keeps the legacy
    // single-thread loop regardless of the flag.
    pipelined_ = kEnablePipelinedInference && detector_->HasPipelined();
    try {
        if (pipelined_) {
            pre_thread_ = std::thread(&SharedDetectorScheduler::PreLoop, this);
            try {
                gpu_thread_ = std::thread(&SharedDetectorScheduler::GpuLoop, this);
            } catch (...) {
                // Unwind the partially-started pre_thread before reporting
                // failure: signal stop, drop pool waiters, join.
                running_.store(false);
                if (detector_) detector_->ClosePipelinedPool();
                pre_to_gpu_q_.Close();
                if (pre_thread_.joinable()) {
                    try { pre_thread_.join(); } catch (...) {}
                }
                throw;
            }
        } else {
            infer_thread_ = std::thread(&SharedDetectorScheduler::InferLoop, this);
        }
    } catch (...) {
        // std::thread ctor throws std::system_error when the OS refuses a new
        // thread. Don't cross the C ABI boundary with a C++ exception.
        try { channels_.clear(); } catch (...) {}
        detector_.reset();
        running_.store(false);
        return false;
    }
    return true;
}

void SharedDetectorScheduler::Stop() {
    if (!running_.exchange(false)) return;

    // Stage 1: ask each channel to stop. Only closes queues / clears
    // has_params_ / requests MmfReader stop — no pointer is invalidated yet,
    // so InferLoop (still running) can finish any in-flight Commit* without
    // UAF.
    for (auto& c : channels_) {
        if (c) {
            try { c->Stop(); } catch (...) {}
        }
    }

    // Stage 2: join the inference thread(s). After this point, no one
    // touches per-channel pool_ / queues / ctx via the scheduler path.
    // Swallow join failures — dtor is implicitly noexcept and we'd hit
    // std::terminate otherwise.
    if (pipelined_) {
        // Pre is the producer side. Wake any thread blocked inside
        // Phase1Prepare's pool acquire so PreLoop can observe running_=false
        // and exit; only then close the pre->gpu queue so GpuLoop drains
        // any in-flight items the PreLoop pushed before noticing the stop.
        if (detector_) {
            try { detector_->ClosePipelinedPool(); } catch (...) {}
        }
        if (pre_thread_.joinable()) {
            try { pre_thread_.join(); }
            catch (const std::exception& e) {
                std::cerr << "[scheduler] pre_thread join failed: " << e.what() << std::endl;
            }
            catch (...) {}
        }
        pre_to_gpu_q_.Close();
        if (gpu_thread_.joinable()) {
            try { gpu_thread_.join(); }
            catch (const std::exception& e) {
                std::cerr << "[scheduler] gpu_thread join failed: " << e.what() << std::endl;
            }
            catch (...) {}
        }
    } else if (infer_thread_.joinable()) {
        try { infer_thread_.join(); }
        catch (const std::exception& e) {
            std::cerr << "[scheduler] infer_thread join failed: " << e.what() << std::endl;
        }
        catch (...) {}
    }

    // Stage 3: now safe to Join channels (which resets their pool_ / queues /
    // ctx).
    for (auto& c : channels_) {
        if (c) {
            try { c->Join(); } catch (...) {}
        }
    }

    try { channels_.clear(); } catch (...) {}
    detector_.reset();
}

ChannelPipeline* SharedDetectorScheduler::FindByPort(int port) {
    for (auto& c : channels_) {
        if (c && c->Port() == port) return c.get();
    }
    return nullptr;
}

ChannelPipeline* SharedDetectorScheduler::ChannelAt(std::size_t i) {
    if (i >= channels_.size()) return nullptr;
    return channels_[i].get();
}

std::string SharedDetectorScheduler::Backend() const {
    return detector_ ? detector_->Backend() : std::string();
}

ChannelPipeline* SharedDetectorScheduler::PickNextChannel() {
    const std::size_t N = channels_.size();
    if (N == 0) return nullptr;
    for (std::size_t i = 0; i < N; ++i) {
        const std::size_t idx = (rr_cursor_ + i) % N;
        ChannelPipeline* c = channels_[idx].get();
        if (c && c->HasParams() && c->PendingInferDepth() > 0) {
            rr_cursor_ = (idx + 1) % N;
            return c;
        }
    }
    return nullptr;
}

void SharedDetectorScheduler::InferLoop() {
    using namespace std::chrono;

    while (running_.load(std::memory_order_acquire)) {
        ChannelPipeline* chan = PickNextChannel();
        if (!chan) {
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }

        FrameSlot* slot = nullptr;
        ParamSnapshot::View view;
        if (!chan->TryAcquireWork(slot, view)) continue;

        if (kEnableTimingLog) TimingRecorder::Instance().MarkInferQueueOut(slot->timestamp);

        // HasParams gates on 'pool sized', not 'ROI configured' — the first
        // /SetParameters can lock resolution while leaving rois[] empty, so
        // this guard must stay even though PickNextChannel filtered HasParams.
        if (view.roi_rects.empty()) {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::SkippedRoiEmpty);
            chan->CommitEmpty(slot);
            continue;
        }

        // Null-ctx guard at the caller so future IDetector adapters don't
        // have to repeat the check. detector_factory.cpp's current adapters
        // also guard internally; this is belt-and-braces.
        if (!chan->Context()) {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::SkippedCtxNull);
            chan->CommitEmpty(slot);
            continue;
        }

        ScaleViewToFrame(view, slot->width, slot->height);

        DetectionResult result;
        int n = 0;
        if (kEnableTimingLog) TimingRecorder::Instance().SetInferTimestamp(slot->timestamp);
        try {
            n = detector_->Detect(
                chan->Context(),
                slot->data.data(), slot->width, slot->height,
                view.roi_rects.data(), static_cast<int>(view.roi_rects.size()),
                view.original_roi_points,
                view.params,
                result);
        } catch (...) {
            // spec §9: detector exception must not take the channel down.
            if (kEnableTimingLog) {
                TimingRecorder::Instance().ClearInferTimestamp();
                TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::DroppedDetectError);
            }
            chan->CommitError(slot);
            continue;
        }
        if (kEnableTimingLog) {
            TimingRecorder::Instance().ClearInferTimestamp();
            TimingRecorder::Instance().MarkDetectDone(slot->timestamp);
        }

        if (n > 0) {
            chan->CommitResult(slot,
                               std::move(result.flattened_points),
                               result.rois_count,
                               result.node_count);
        } else {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::SkippedNoDetection);
            chan->CommitEmpty(slot);
        }
    }
}

// ---------- Pipelined (route B) -------------------------------------------
// PreLoop owns the same round-robin pick as InferLoop and the same view/ROI
// guards, but stops at Phase1Prepare. Each prepared frame becomes a
// PreToGpuItem on pre_to_gpu_q_; GpuLoop downstream runs Phase2Gpu +
// Phase3Post + CommitResult. Phase1Prepare returning -1 (stride-skipped or
// invalid input) still pushes a queue item so the channel's dispatch_q sees
// frames in order — GpuLoop treats detector_slot<0 as a CommitEmpty shortcut.

void SharedDetectorScheduler::PreLoop() {
    using namespace std::chrono;

    while (running_.load(std::memory_order_acquire)) {
        ChannelPipeline* chan = PickNextChannel();
        if (!chan) {
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }

        FrameSlot* slot = nullptr;
        ParamSnapshot::View view;
        if (!chan->TryAcquireWork(slot, view)) continue;

        if (kEnableTimingLog) TimingRecorder::Instance().MarkInferQueueOut(slot->timestamp);

        if (view.roi_rects.empty()) {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::SkippedRoiEmpty);
            chan->CommitEmpty(slot);
            continue;
        }
        if (!chan->Context()) {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::SkippedCtxNull);
            chan->CommitEmpty(slot);
            continue;
        }

        ScaleViewToFrame(view, slot->width, slot->height);

        int dslot = -1;
        if (kEnableTimingLog) TimingRecorder::Instance().SetInferTimestamp(slot->timestamp);
        try {
            dslot = detector_->Phase1Prepare(
                chan->Context(),
                slot->data.data(), slot->width, slot->height,
                view.roi_rects.data(), static_cast<int>(view.roi_rects.size()),
                view.original_roi_points,
                view.params);
        } catch (...) {
            if (kEnableTimingLog) {
                TimingRecorder::Instance().ClearInferTimestamp();
                TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::DroppedDetectError);
            }
            chan->CommitError(slot);
            continue;
        }
        if (kEnableTimingLog) TimingRecorder::Instance().ClearInferTimestamp();

        if (dslot == -2) {
            // Pool was closed mid-acquire. The shutdown signal: bail out
            // immediately, releasing this slot back to the caller via
            // CommitError so the channel doesn't leak it.
            chan->CommitError(slot);
            break;
        }

        if (kEnableTimingLog) TimingRecorder::Instance().MarkPreToGpuQueueIn(slot->timestamp);

        PreToGpuItem item;
        item.channel = chan;
        item.slot = slot;
        item.view = std::move(view);
        item.detector_slot = dslot;
        item.timestamp = slot->timestamp;

        // Spin-with-sleep instead of blocking push: the queue cap is small
        // and we want to notice running_=false promptly during shutdown.
        // TryPushRef leaves `item` intact on failure — a by-value TryPush
        // would move the view out even when the queue is full, and the
        // retry would push an item with empty roi_rects (ROI filter lost).
        bool pushed = false;
        while (running_.load(std::memory_order_acquire)) {
            if (pre_to_gpu_q_.TryPushRef(item)) {
                pushed = true;
                break;
            }
            std::this_thread::sleep_for(milliseconds(1));
        }
        if (!pushed) {
            // Shutdown while still holding the frame slot — release it so the
            // channel pool stays balanced. The detector pool slot (dslot) is
            // left as-is: ClosePipelinedPool has already been called on this
            // path and the whole pool is being torn down.
            chan->CommitError(slot);
        }
    }
}

void SharedDetectorScheduler::GpuLoop() {
    using namespace std::chrono;

    while (running_.load(std::memory_order_acquire)) {
        PreToGpuItem item;
        if (!pre_to_gpu_q_.PopWait(item, milliseconds(100))) {
            // Either a 100ms idle timeout (loop back to re-check running_)
            // or the queue is empty and closed (drain done, exit).
            continue;
        }

        if (kEnableTimingLog) TimingRecorder::Instance().MarkPreToGpuQueueOut(item.timestamp);

        // Stride-skipped (or invalid input) frames carry detector_slot < 0
        // so GpuLoop bypasses the detector entirely; the dispatch_q still
        // gets a CommitEmpty in arrival order, preserving per-channel FIFO.
        if (item.detector_slot < 0) {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(item.timestamp, TimingRecorder::FrameState::SkippedNoDetection);
            item.channel->CommitEmpty(item.slot);
            continue;
        }

        if (kEnableTimingLog) TimingRecorder::Instance().SetInferTimestamp(item.timestamp);
        try {
            detector_->Phase2Gpu(item.channel->Context(), item.detector_slot);
        } catch (...) {
            // Phase2Gpu released the detector pool slot before rethrowing.
            if (kEnableTimingLog) {
                TimingRecorder::Instance().ClearInferTimestamp();
                TimingRecorder::Instance().Flush(item.timestamp, TimingRecorder::FrameState::DroppedDetectError);
            }
            item.channel->CommitError(item.slot);
            continue;
        }

        DetectionResult result;
        int n = 0;
        try {
            n = detector_->Phase3Post(
                item.channel->Context(), item.detector_slot,
                item.view.roi_rects.data(),
                static_cast<int>(item.view.roi_rects.size()),
                item.view.original_roi_points,
                item.view.params,
                result);
        } catch (...) {
            // Phase3Post released the detector pool slot before rethrowing.
            if (kEnableTimingLog) {
                TimingRecorder::Instance().ClearInferTimestamp();
                TimingRecorder::Instance().Flush(item.timestamp, TimingRecorder::FrameState::DroppedDetectError);
            }
            item.channel->CommitError(item.slot);
            continue;
        }
        if (kEnableTimingLog) {
            TimingRecorder::Instance().ClearInferTimestamp();
            TimingRecorder::Instance().MarkDetectDone(item.timestamp);
        }

        if (n > 0) {
            item.channel->CommitResult(item.slot,
                                       std::move(result.flattened_points),
                                       result.rois_count,
                                       result.node_count);
        } else {
            if (kEnableTimingLog) TimingRecorder::Instance().Flush(item.timestamp, TimingRecorder::FrameState::SkippedNoDetection);
            item.channel->CommitEmpty(item.slot);
        }
    }

    // Final drain after running_ flipped: pop anything left in the queue and
    // release the slots through CommitError so channel pools don't leak.
    PreToGpuItem residual;
    while (pre_to_gpu_q_.PopWait(residual, std::chrono::milliseconds(0))) {
        if (residual.channel && residual.slot) {
            residual.channel->CommitError(residual.slot);
        }
    }
}

}  // namespace gai
