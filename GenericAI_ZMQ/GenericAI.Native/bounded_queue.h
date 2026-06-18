#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

namespace gai {

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

    bool TryPush(T item) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (closed_ || q_.size() >= capacity_) return false;
        q_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    // Like TryPush, but only consumes `item` on success. If the queue is
    // full or closed, `item` is left intact so the caller can retry the
    // same payload without rebuilding it. Used by the spin-with-sleep
    // backpressure paths (MmfReader → infer_q, CommitResult → dispatch_q)
    // where a heavy payload (e.g. std::vector<GAI_Roi>) must survive
    // across retries.
    bool TryPushRef(T& item) {
        std::lock_guard<std::mutex> lk(mtx_);
        if (closed_ || q_.size() >= capacity_) return false;
        q_.push(std::move(item));
        cv_.notify_one();
        return true;
    }

    template <typename Rep, typename Period>
    bool PopWait(T& out, std::chrono::duration<Rep, Period> timeout) {
        std::unique_lock<std::mutex> lk(mtx_);
        if (!cv_.wait_for(lk, timeout, [&] { return closed_ || !q_.empty(); }))
            return false;
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void Close() {
        std::lock_guard<std::mutex> lk(mtx_);
        closed_ = true;
        cv_.notify_all();
    }

    std::size_t Size() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return q_.size();
    }

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::queue<T> q_;
    std::size_t capacity_;
    bool closed_ = false;
};

}  // namespace gai
