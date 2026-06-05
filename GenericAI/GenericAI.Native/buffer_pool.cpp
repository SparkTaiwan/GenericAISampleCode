#include "pch.h"
#include "buffer_pool.h"

namespace gai {

BufferPool::BufferPool(std::size_t num_slots, std::size_t per_slot_capacity)
    : per_slot_capacity_(per_slot_capacity) {
    slots_.reserve(num_slots);
    free_.reserve(num_slots);
    for (std::size_t i = 0; i < num_slots; ++i) {
        auto slot = std::unique_ptr<FrameSlot>(new FrameSlot());
        slot->data.resize(per_slot_capacity);
        free_.push_back(slot.get());
        slots_.push_back(std::move(slot));
    }
}

FrameSlot* BufferPool::Acquire() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (free_.empty()) return nullptr;
    FrameSlot* slot = free_.back();
    free_.pop_back();
    return slot;
}

void BufferPool::Release(FrameSlot* slot) {
    if (!slot) return;
    std::lock_guard<std::mutex> lk(mtx_);
    free_.push_back(slot);
}

}  // namespace gai
