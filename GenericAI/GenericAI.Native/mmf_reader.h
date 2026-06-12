#pragma once

#include "pch.h"
#include "bounded_queue.h"
#include "buffer_pool.h"

#include <atomic>
#include <cstdint>
#include <thread>

namespace gai {

// Reads frames from MMF "ChannelFrame_<port>" written by spark.recorder.
// Layout matches the legacy MMF_Data (header 0x1234, footer 0x4321,
// status 0=idle 1=new 2=consumed) so spark.recorder side stays unchanged.
//
// On status==1: copy bytes into a BufferPool slot, set status=2, push slot
// into the infer queue. If the pool is exhausted, leave status=1 (no ack),
// sleep briefly and retry — recorder side sees status unchanged and applies
// its own backpressure. Same shape on infer-queue push failure. Each retry
// iteration is counted in pool_stall_count_; the wrapper never drops a frame
// of its own accord.
class MmfReader {
public:
    MmfReader(int port, BufferPool& pool, BoundedQueue<FrameSlot*>& out);
    ~MmfReader();

    MmfReader(const MmfReader&) = delete;
    MmfReader& operator=(const MmfReader&) = delete;

    bool Start();
    void RequestStop();
    void Join();

    // Diagnostics (lock-free atomic reads).
    std::uint64_t FramesRead() const { return frames_read_.load(std::memory_order_relaxed); }
    // Number of stall events (pool full OR infer-queue push failure), not a
    // frame count — the same frame can stall in both stages and contribute twice.
    std::uint64_t PoolStallCount() const { return pool_stall_count_.load(std::memory_order_relaxed); }

private:
    void Run();
    bool EnsureMapped();
    void Unmap();

    int port_;
    BufferPool& pool_;
    BoundedQueue<FrameSlot*>& out_;

    HANDLE map_handle_ = INVALID_HANDLE_VALUE;
    void* mapped_view_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread thread_;

    std::atomic<std::uint64_t> frames_read_{0};
    std::atomic<std::uint64_t> pool_stall_count_{0};
};

}  // namespace gai
