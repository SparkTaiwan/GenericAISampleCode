#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>

namespace gai_host {

// Bounded producer/consumer queue with the BlockingCollection<T> semantics the
// C# host relies on:
//   Add          blocks while full; returns false once CompleteAdding() ran
//                (C# throws InvalidOperationException there).
//   TryTake      never blocks.
//   IsCompleted  CompleteAdding() ran AND the queue is drained -> no more items
//                will ever arrive.
template <typename T>
class BlockingQueue {
public:
    explicit BlockingQueue(std::size_t capacity) : capacity_(capacity) {}

    BlockingQueue(const BlockingQueue&) = delete;
    BlockingQueue& operator=(const BlockingQueue&) = delete;

    bool Add(T&& item) {
        std::unique_lock<std::mutex> lk(mtx_);
        not_full_.wait(lk, [&] { return completed_ || items_.size() < capacity_; });
        if (completed_) return false;
        items_.push_back(std::move(item));
        return true;
    }

    bool TryTake(T& out) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (items_.empty()) return false;
        out = std::move(items_.front());
        items_.pop_front();
        not_full_.notify_one();
        return true;
    }

    void CompleteAdding() {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            completed_ = true;
        }
        not_full_.notify_all();
    }

    bool IsCompleted() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return completed_ && items_.empty();
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mtx_;
    std::condition_variable not_full_;
    std::deque<T> items_;
    bool completed_ = false;
};

}  // namespace gai_host
