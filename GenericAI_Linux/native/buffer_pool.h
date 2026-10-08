#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace gai {

struct FrameSlot {
    std::vector<unsigned char> data;   // pre-sized to capacity at construction
    int width = 0;
    int height = 0;
    int size = 0;                      // actual bytes used
    std::uint64_t timestamp = 0;
};

// Fixed-size pool of pre-allocated frame slots. Acquire returns nullptr when
// every slot is in flight; callers MUST drop the frame in that case (do not
// block, do not allocate ad hoc — that's how leaks crept into the old code).
class BufferPool {
public:
    BufferPool(std::size_t num_slots, std::size_t per_slot_capacity);

    FrameSlot* Acquire();
    void Release(FrameSlot* slot);

    std::size_t Capacity() const { return slots_.size(); }
    std::size_t SlotCapacity() const { return per_slot_capacity_; }

private:
    std::size_t per_slot_capacity_;
    std::vector<std::unique_ptr<FrameSlot>> slots_;
    std::vector<FrameSlot*> free_;
    std::mutex mtx_;
};

}  // namespace gai
