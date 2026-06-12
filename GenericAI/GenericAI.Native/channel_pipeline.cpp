#include "pch.h"
#include "channel_pipeline.h"
#include "gai_config.h"
#include "host_log.h"
#include "timing_recorder.h"

#include <chrono>
#include <iostream>
#include <string>
#include <utility>

namespace gai {

namespace {
// Same family of trade-off as C# ChannelHandle's EncodeQueueCapacity / SendQueueCapacity:
// small caps keep stop-stream-to-callback-stop latency tight at the cost of less burst absorption.
constexpr std::size_t kPoolSlots        = 2;
constexpr std::size_t kInferQueueCap    = 2;
constexpr std::size_t kDispatchQueueCap = 2;
}  // namespace

ChannelPipeline::ChannelPipeline(int port) : port_(port) {}

ChannelPipeline::~ChannelPipeline() {
    Stop();
    Join();
}

void ChannelPipeline::Start() {
    if (running_.exchange(true)) return;

    infer_q_    = std::unique_ptr<BoundedQueue<FrameSlot*>>(
                    new BoundedQueue<FrameSlot*>(kInferQueueCap));
    dispatch_q_ = std::unique_ptr<BoundedQueue<ChannelDetectionPacket>>(
                    new BoundedQueue<ChannelDetectionPacket>(kDispatchQueueCap));

    dispatch_thread_ = std::thread(&ChannelPipeline::DispatchLoop, this);
}

void ChannelPipeline::Stop() {
    if (!running_.exchange(false)) return;

    // Clear has_params_ first so the scheduler stops picking this channel
    // before we touch the reader / queues. Eliminates the wasted-poll window.
    has_params_.store(false, std::memory_order_release);

    if (mmf_reader_) mmf_reader_->RequestStop();
    if (infer_q_)    infer_q_->Close();
    if (dispatch_q_) dispatch_q_->Close();
}

void ChannelPipeline::Join() {
    if (mmf_reader_)              mmf_reader_->Join();
    if (dispatch_thread_.joinable()) dispatch_thread_.join();

    mmf_reader_.reset();
    infer_q_.reset();
    dispatch_q_.reset();
    pool_.reset();
    detector_ctx_.reset();
}

void ChannelPipeline::ApplyParameters(const GAI_Settings& s) {
    params_.Apply(s);

    std::lock_guard<std::mutex> lk(start_mtx_);
    // has_params_ doubles as the first-time-init gate while we hold start_mtx_:
    // outside the lock it is a lock-free flag for the scheduler.
    if (has_params_.load(std::memory_order_relaxed)) {
        if (s.image_width > 0 && s.image_height > 0 &&
            pool_ &&
            static_cast<std::size_t>(s.image_width) * s.image_height * 3 / 2 != pool_->SlotCapacity()) {
            // Unconditional: the pool stays at the first resolution, so frames
            // larger than it will be dropped wholesale by MmfReader — the
            // operator needs to see why detection went quiet.
            const std::string msg =
                "[channel " + std::to_string(port_) + "] SetParameters reports " +
                std::to_string(s.image_width) + "x" + std::to_string(s.image_height) +
                " but pool is locked to the first resolution; ignoring resize."
                " Restart the channel to change resolution.";
            std::cerr << msg << std::endl;
            HostLog(HostLogLevel::Warn, msg);
        }
        return;
    }
    if (s.image_width <= 0 || s.image_height <= 0) return;
    if (!infer_q_) return;

    const std::size_t cap =
        static_cast<std::size_t>(s.image_width) * s.image_height * 3 / 2;
    pool_       = std::unique_ptr<BufferPool>(new BufferPool(kPoolSlots, cap));
    mmf_reader_ = std::unique_ptr<MmfReader>(new MmfReader(port_, *pool_, *infer_q_));
    mmf_reader_->Start();
    has_params_.store(true, std::memory_order_release);

    // Host log gets this unconditionally: the locked pool capacity is what
    // every later "dropping frames" warning compares against.
    const std::string msg =
        "[channel " + std::to_string(port_) + "] pool sized to " +
        std::to_string(s.image_width) + "x" + std::to_string(s.image_height) +
        " (" + std::to_string(kPoolSlots) + " slots x " + std::to_string(cap) +
        " bytes = " + std::to_string(kPoolSlots * cap) + " bytes)";
    HostLog(HostLogLevel::Info, msg);
    if (kEnableTimingLog) {
        std::cout << msg << std::endl;
    }
}

void ChannelPipeline::SetCallback(GAI_DetectionCallback cb) {
    callback_.store(cb, std::memory_order_release);
}

void ChannelPipeline::InitContext(IDetector* shared_detector) {
    if (!shared_detector) return;
    detector_ctx_ = shared_detector->CreateContext();
}

bool ChannelPipeline::TryAcquireWork(FrameSlot*& slot, ParamSnapshot::View& view) {
    slot = nullptr;
    if (!infer_q_) return false;
    FrameSlot* s = nullptr;
    if (!infer_q_->PopWait(s, std::chrono::milliseconds(0))) return false;
    if (!s) return false;
    slot = s;
    view = params_.Take();
    return true;
}

void ChannelPipeline::CommitResult(FrameSlot* slot, std::vector<GAI_Roi>&& flat,
                                   int rois_count, int node_count) {
    using namespace std::chrono;
    if (!slot) return;
    const std::uint64_t ts = slot->timestamp;
    ChannelDetectionPacket pkt;
    pkt.frame      = slot;
    pkt.rois_flat  = std::move(flat);
    pkt.rois_count = rois_count;
    pkt.node_count = node_count;

    // Spin-with-sleep instead of dropping: pressure backs up through this
    // channel's dispatch_q -> BufferPool -> MmfReader -> share memory
    // writer. Caller (InferLoop / GpuLoop) is a single shared thread, so
    // this also stalls every other channel's detection -- accepted by
    // design: a wedged callback halts the whole detector and resumes
    // when the downstream drains. TryPushRef leaves `pkt` intact on
    // failure so we can retry the same payload.
    while (running_.load(std::memory_order_acquire)) {
        if (dispatch_q_ && dispatch_q_->TryPushRef(pkt)) {
            if (kEnableTimingLog) TimingRecorder::Instance().MarkDispatchQueueIn(ts);
            return;
        }
        std::this_thread::sleep_for(milliseconds(1));
    }
    // Shutdown while still holding the slot -- release to avoid leak.
    if (pool_) pool_->Release(slot);
}

void ChannelPipeline::CommitEmpty(FrameSlot* slot) {
    if (!slot) return;
    if (pool_) pool_->Release(slot);
}

void ChannelPipeline::CommitError(FrameSlot* slot) {
    if (!slot) return;
    if (pool_) pool_->Release(slot);
}

void ChannelPipeline::DispatchLoop() {
    using namespace std::chrono;

    while (running_.load(std::memory_order_acquire)) {
        ChannelDetectionPacket pkt;
        if (!dispatch_q_ || !dispatch_q_->PopWait(pkt, milliseconds(100))) continue;
        if (!pkt.frame) continue;

        if (kEnableTimingLog) {
            TimingRecorder::Instance().MarkDispatchQueueOut(pkt.frame->timestamp);
            TimingRecorder::Instance().Flush(pkt.frame->timestamp, TimingRecorder::FrameState::Ok);
        }

        if (pkt.rois_count > 0) {
            auto cb = callback_.load(std::memory_order_acquire);
            if (cb) {
                cb(port_,
                   pkt.frame->width, pkt.frame->height,
                   pkt.frame->data.data(), pkt.frame->size,
                   pkt.frame->timestamp,
                   pkt.rois_flat.data(), pkt.rois_count, pkt.node_count);
            }
        }

        if (pool_) pool_->Release(pkt.frame);
    }
}

}  // namespace gai
