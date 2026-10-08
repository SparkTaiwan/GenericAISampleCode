#include "channel_handle.h"
#include "http_control_server.h"

namespace gai_host {

ChannelHandle::ChannelHandle(int port, const std::string& host)
    : port_(port),
      listener_(new HttpControlServer(port, params_, host)) {}

ChannelHandle::~ChannelHandle() = default;

bool ChannelHandle::StartListener() { return listener_->Start(); }

void ChannelHandle::StopListener() { listener_->Stop(); }

bool ChannelHandle::TryPassTriggerInterval(int interval_sec) {
    if (interval_sec <= 0) return true;
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const std::int64_t window = static_cast<std::int64_t>(interval_sec) * 1000000000LL;
    std::int64_t last = last_trigger_ns_.load();
    while (true) {
        if (last != 0 && (now - last) < window) return false;  // still inside the quiet window
        if (last_trigger_ns_.compare_exchange_weak(last, now)) return true;
        // Lost the race (or spurious failure): `last` was reloaded, re-check.
    }
}

bool ChannelHandle::HasParkedSend() const {
    // A claimed payload is out of parked_ while its retry is in flight; it
    // still blocks the channel (the C# version only checked the queue, which
    // let another worker take a fresh envelope during that retry).
    std::lock_guard<std::mutex> lk(park_mtx_);
    return !parked_.empty() || retry_claimed_.load();
}

void ChannelHandle::ParkSend(std::string payload, std::chrono::milliseconds backoff) {
    std::lock_guard<std::mutex> lk(park_mtx_);
    parked_.push_back(std::move(payload));
    retry_at_ = std::chrono::steady_clock::now() + backoff;
}

// Claims the next parked payload for one retry attempt. False when nothing is
// parked, the backoff has not elapsed, or another worker holds the claim.
bool ChannelHandle::TryClaimParkedSend(std::string& payload) {
    std::lock_guard<std::mutex> lk(park_mtx_);
    if (parked_.empty()) return false;
    if (std::chrono::steady_clock::now() < retry_at_) return false;
    bool expected = false;
    if (!retry_claimed_.compare_exchange_strong(expected, true)) return false;
    payload = std::move(parked_.front());
    parked_.pop_front();
    return true;
}

// Releases the claim; on failure the payload goes back with a fresh backoff.
void ChannelHandle::CompleteParkedSend(std::string payload, bool success,
                                       std::chrono::milliseconds backoff) {
    if (!success) ParkSend(std::move(payload), backoff);
    retry_claimed_.store(false);
}

}  // namespace gai_host
