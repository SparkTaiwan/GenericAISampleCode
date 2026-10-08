#include "pch.h"
#include "timing_recorder.h"
#include "gai_config.h"

#include <cstdio>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <vector>

namespace gai {

namespace {

constexpr std::chrono::seconds kSweepInterval{1};
constexpr std::chrono::seconds kStaleAge{10};
constexpr const char* kLinePrefix = "[TIMING-CPP]";

// Holds the timestamp of the frame currently being processed on the InferLoop
// thread, so detector internals (RunYolox / RunYolov6 / RunDamoYolo) can mark
// their CPU pre / GPU / CPU post sub-segments without taking a timestamp
// parameter through the IDetector::Detect signature.
thread_local std::uint64_t g_infer_ts = 0;

double ElapsedMs(std::chrono::steady_clock::time_point a,
                 std::chrono::steady_clock::time_point b) {
    auto diff = std::chrono::duration_cast<std::chrono::microseconds>(b - a).count();
    return static_cast<double>(diff) / 1000.0;
}

void AppendField(std::ostringstream& os, const char* name, bool has_start, bool has_end,
                 std::chrono::steady_clock::time_point start,
                 std::chrono::steady_clock::time_point end) {
    os << ' ' << name << '=';
    if (has_start && has_end) {
        os << std::fixed << std::setprecision(3) << ElapsedMs(start, end);
    }
}

// Like AppendField but suppresses the column entirely when neither endpoint
// was marked. Used for fields that only fire in one topology (e.g. the
// pre->gpu queue exists only when kEnablePipelinedInference == true).
void AppendFieldIfAny(std::ostringstream& os, const char* name,
                      bool has_start, bool has_end,
                      std::chrono::steady_clock::time_point start,
                      std::chrono::steady_clock::time_point end) {
    if (!has_start && !has_end) return;
    AppendField(os, name, has_start, has_end, start, end);
}

}  // namespace

TimingRecorder& TimingRecorder::Instance() {
    static TimingRecorder s_instance;
    return s_instance;
}

TimingRecorder::~TimingRecorder() {
    Shutdown();
}

void TimingRecorder::Init(int /*basePort*/) {
    if (initialized_) return;
    initialized_ = true;

    if (!kEnableTimingLog) return;

    enabled_.store(true, std::memory_order_release);
    running_.store(true, std::memory_order_release);
    sweeper_thread_ = std::thread(&TimingRecorder::SweeperLoop, this);
}

void TimingRecorder::Shutdown() {
    if (!running_.exchange(false)) return;
    enabled_.store(false, std::memory_order_release);

    if (sweeper_thread_.joinable()) {
        try { sweeper_thread_.join(); } catch (...) {}
    }

    std::vector<std::pair<std::uint64_t, FrameRecord>> leftover;
    {
        std::lock_guard<std::mutex> lk(map_mtx_);
        leftover.reserve(records_.size());
        for (auto& kv : records_) leftover.push_back(kv);
        records_.clear();
    }
    for (auto& kv : leftover) {
        WriteLine(kv.first, kv.second, FrameState::IncompleteSwept);
    }
}

void TimingRecorder::MarkRead(std::uint64_t timestamp, int port) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    FrameRecord& r = records_[timestamp];
    r.created = now;
    r.port = port;
    r.t_mmf = now;
    r.has_mmf = true;
}

void TimingRecorder::MarkInferQueueIn(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_inferq_in = now;
    it->second.has_inferq_in = true;
}

void TimingRecorder::MarkInferQueueOut(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_inferq_out = now;
    it->second.has_inferq_out = true;
}

void TimingRecorder::MarkPreToGpuQueueIn(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_pre_to_gpu_q_in = now;
    it->second.has_pre_to_gpu_q_in = true;
}

void TimingRecorder::MarkPreToGpuQueueOut(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_pre_to_gpu_q_out = now;
    it->second.has_pre_to_gpu_q_out = true;
}

void TimingRecorder::MarkDetectDone(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_detect_done = now;
    it->second.has_detect_done = true;
}

void TimingRecorder::SetInferTimestamp(std::uint64_t timestamp) {
    g_infer_ts = timestamp;
}

void TimingRecorder::ClearInferTimestamp() {
    g_infer_ts = 0;
}

void TimingRecorder::MarkDetectPreDone() {
    if (!enabled_.load(std::memory_order_acquire)) return;
    std::uint64_t ts = g_infer_ts;
    if (ts == 0) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(ts);
    if (it == records_.end()) return;
    it->second.t_detect_pre_done = now;
    it->second.has_detect_pre_done = true;
}

void TimingRecorder::MarkDetectGpuDone() {
    if (!enabled_.load(std::memory_order_acquire)) return;
    std::uint64_t ts = g_infer_ts;
    if (ts == 0) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(ts);
    if (it == records_.end()) return;
    it->second.t_detect_gpu_done = now;
    it->second.has_detect_gpu_done = true;
}

void TimingRecorder::MarkDispatchQueueIn(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_dispatchq_in = now;
    it->second.has_dispatchq_in = true;
}

void TimingRecorder::MarkDispatchQueueOut(std::uint64_t timestamp) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lk(map_mtx_);
    auto it = records_.find(timestamp);
    if (it == records_.end()) return;
    it->second.t_dispatchq_out = now;
    it->second.has_dispatchq_out = true;
}

void TimingRecorder::Flush(std::uint64_t timestamp, FrameState state) {
    if (!enabled_.load(std::memory_order_acquire)) return;
    FrameRecord rec;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(map_mtx_);
        auto it = records_.find(timestamp);
        if (it != records_.end()) {
            rec = it->second;
            records_.erase(it);
            found = true;
        }
    }
    if (!found) return;
    WriteLine(timestamp, rec, state);
}

void TimingRecorder::SweeperLoop() {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(kSweepInterval);
        auto now = std::chrono::steady_clock::now();

        std::vector<std::pair<std::uint64_t, FrameRecord>> stale;
        {
            std::lock_guard<std::mutex> lk(map_mtx_);
            for (auto it = records_.begin(); it != records_.end(); ) {
                if (now - it->second.created > kStaleAge) {
                    stale.emplace_back(it->first, it->second);
                    it = records_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& kv : stale) {
            WriteLine(kv.first, kv.second, FrameState::IncompleteSwept);
        }
    }
}

void TimingRecorder::WriteLine(std::uint64_t timestamp, const FrameRecord& r, FrameState state) {
    // Suppress lines that would print zero segment values — pool/inferq full
    // drops have only t_mmf and nothing else, so every s_xxx column would be
    // blank. Pool exhaustion is a stall rather than a true drop and is counted
    // by MmfReader::pool_stall_count_.
    bool any_segment = r.has_inferq_in || r.has_inferq_out || r.has_detect_done ||
                       r.has_dispatchq_in || r.has_dispatchq_out;
    if (!any_segment) return;

    std::ostringstream os;
    os << kLinePrefix
       << " [stamp=" << timestamp
       << " port=" << r.port
       << " state=" << StateName(state) << "]";
    AppendField(os, "s_mmf_to_inferq",  r.has_mmf,             r.has_inferq_in,       r.t_mmf,             r.t_inferq_in);
    AppendField(os, "s_wait_infer",     r.has_inferq_in,       r.has_inferq_out,      r.t_inferq_in,       r.t_inferq_out);
    AppendField(os, "s_ai",             r.has_inferq_out,      r.has_detect_done,     r.t_inferq_out,      r.t_detect_done);
    AppendField(os, "s_ai_pre",         r.has_inferq_out,      r.has_detect_pre_done, r.t_inferq_out,      r.t_detect_pre_done);
    // Pipelined-only: pre->gpu queue wait between PreLoop's MarkPreToGpuQueueIn
    // (immediately after MarkDetectPreDone fires inside Phase1Prepare) and
    // GpuLoop's MarkPreToGpuQueueOut (immediately before Phase2Gpu runs).
    // Column omitted entirely in single-thread mode (neither mark fires).
    AppendFieldIfAny(os, "s_wait_gpu_q",
                     r.has_pre_to_gpu_q_in, r.has_pre_to_gpu_q_out,
                     r.t_pre_to_gpu_q_in,   r.t_pre_to_gpu_q_out);
    // In pipelined mode s_ai_gpu measures session.Run alone, starting at the
    // pre->gpu queue exit so the column doesn't absorb queue-wait time.
    // In single-thread mode there is no queue; fall back to t_detect_pre_done.
    {
        const bool pipelined = r.has_pre_to_gpu_q_out;
        AppendField(os, "s_ai_gpu",
                    pipelined ? r.has_pre_to_gpu_q_out : r.has_detect_pre_done,
                    r.has_detect_gpu_done,
                    pipelined ? r.t_pre_to_gpu_q_out   : r.t_detect_pre_done,
                    r.t_detect_gpu_done);
    }
    AppendField(os, "s_ai_post",        r.has_detect_gpu_done, r.has_detect_done,     r.t_detect_gpu_done, r.t_detect_done);
    AppendField(os, "s_to_dispatchq",   r.has_detect_done,     r.has_dispatchq_in,    r.t_detect_done,     r.t_dispatchq_in);
    AppendField(os, "s_wait_dispatch",  r.has_dispatchq_in,    r.has_dispatchq_out,   r.t_dispatchq_in,    r.t_dispatchq_out);
    AppendField(os, "cpp_total",        r.has_mmf,             r.has_dispatchq_out,   r.t_mmf,             r.t_dispatchq_out);

    std::lock_guard<std::mutex> lk(print_mtx_);
    std::cout << os.str() << std::endl;
}

const char* TimingRecorder::StateName(FrameState s) {
    switch (s) {
        case FrameState::Ok:                   return "OK";
        case FrameState::DroppedPoolFull:      return "dropped_pool_full";
        case FrameState::DroppedInferQFull:    return "dropped_inferq_full";
        case FrameState::SkippedRoiEmpty:      return "skipped_roi_empty";
        case FrameState::SkippedCtxNull:       return "skipped_ctx_null";
        case FrameState::SkippedNoDetection:   return "skipped_no_detection";
        case FrameState::DroppedDetectError:   return "dropped_detect_error";
        case FrameState::DroppedDispatchQFull: return "dropped_dispatchq_full";
        case FrameState::IncompleteSwept:      return "incomplete_swept";
        case FrameState::InProgress:           return "in_progress";
        default:                               return "unknown";
    }
}

}  // namespace gai
