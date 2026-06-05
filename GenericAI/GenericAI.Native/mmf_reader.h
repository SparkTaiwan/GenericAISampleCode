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
// into the infer queue. If the pool is exhausted, still set status=2 so the
// recorder is not blocked; the frame is simply dropped.
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
    std::uint64_t FramesDropped() const { return frames_dropped_.load(std::memory_order_relaxed); }

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
    std::atomic<std::uint64_t> frames_dropped_{0};
};

}  // namespace gai
