#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace gai {

// Per-frame timing recorder (singleton). Records timestamps at 6 points in the
// C++ pipeline, writes one console line per frame when the dispatch loop hands
// the frame off across the P/Invoke boundary (or earlier on any drop path).
// The matching C# side has its own TimingRecorder also printing to console;
// the two console lines are correlated by the frame's MMF timestamp.
//
// Temporary Step 10/11 instrumentation. Toggle via gai_config.h
// kEnableTimingLog; when false, Init() leaves enabled_ = false and every Mark
// call short-circuits at the first line.
class TimingRecorder {
public:
    static TimingRecorder& Instance();

    enum class FrameState {
        InProgress,
        Ok,
        DroppedPoolFull,
        DroppedInferQFull,
        SkippedRoiEmpty,
        SkippedCtxNull,
        SkippedNoDetection,
        DroppedDetectError,
        DroppedDispatchQFull,
        IncompleteSwept,
    };

    // basePort is kept in the signature for parity with the C# side, but the
    // C++ recorder no longer opens a file — it just starts the sweeper.
    void Init(int basePort);
    void Shutdown();

    void MarkRead(std::uint64_t timestamp, int port);
    void MarkInferQueueIn(std::uint64_t timestamp);
    void MarkInferQueueOut(std::uint64_t timestamp);
    // Pipelined route only: pre→gpu queue boundary so s_wait_gpu_q can be
    // measured. In single-thread mode these are never called and the field
    // is omitted from the printed line.
    void MarkPreToGpuQueueIn(std::uint64_t timestamp);
    void MarkPreToGpuQueueOut(std::uint64_t timestamp);
    void MarkDetectDone(std::uint64_t timestamp);
    void MarkDispatchQueueIn(std::uint64_t timestamp);
    void MarkDispatchQueueOut(std::uint64_t timestamp);

    // Detector-internal sub-segment marks (CPU pre / GPU / CPU post within
    // s_ai). Detector code (detector_person RunYolox / RunYolov6 / RunDamoYolo)
    // doesn't know the frame timestamp; the scheduler stashes it in a
    // thread_local via SetInferTimestamp before Detect, and these two Mark
    // calls read from that. Detect simply uses MarkDetectPreDone right after
    // CPU preprocessing and MarkDetectGpuDone right after session.Run.
    void SetInferTimestamp(std::uint64_t timestamp);
    void ClearInferTimestamp();
    void MarkDetectPreDone();
    void MarkDetectGpuDone();

    void Flush(std::uint64_t timestamp, FrameState state);

private:
    TimingRecorder() = default;
    ~TimingRecorder();

    TimingRecorder(const TimingRecorder&) = delete;
    TimingRecorder& operator=(const TimingRecorder&) = delete;

    struct FrameRecord {
        int port = 0;
        std::chrono::steady_clock::time_point created;
        std::chrono::steady_clock::time_point t_mmf;
        std::chrono::steady_clock::time_point t_inferq_in;
        std::chrono::steady_clock::time_point t_inferq_out;
        std::chrono::steady_clock::time_point t_detect_pre_done;
        std::chrono::steady_clock::time_point t_pre_to_gpu_q_in;
        std::chrono::steady_clock::time_point t_pre_to_gpu_q_out;
        std::chrono::steady_clock::time_point t_detect_gpu_done;
        std::chrono::steady_clock::time_point t_detect_done;
        std::chrono::steady_clock::time_point t_dispatchq_in;
        std::chrono::steady_clock::time_point t_dispatchq_out;
        bool has_mmf = false;
        bool has_inferq_in = false;
        bool has_inferq_out = false;
        bool has_detect_pre_done = false;
        bool has_pre_to_gpu_q_in = false;
        bool has_pre_to_gpu_q_out = false;
        bool has_detect_gpu_done = false;
        bool has_detect_done = false;
        bool has_dispatchq_in = false;
        bool has_dispatchq_out = false;
    };

    void SweeperLoop();
    void WriteLine(std::uint64_t timestamp, const FrameRecord& rec, FrameState state);
    static const char* StateName(FrameState s);

    std::mutex map_mtx_;
    std::unordered_map<std::uint64_t, FrameRecord> records_;

    // Serialises the multi-`<<` writes to std::cout so lines from concurrent
    // MmfReader / InferLoop / DispatchThread threads don't interleave.
    std::mutex print_mtx_;

    std::thread sweeper_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> enabled_{false};
    bool initialized_ = false;
};

}  // namespace gai
