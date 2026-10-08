#pragma once

#include "blocking_queue.h"
#include "gai_abi.h"
#include "state.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace gai_host {

class HttpControlServer;

// One detection handed from the native callback to the encode pool.
struct RawDetection {
    int width = 0;
    int height = 0;
    std::uint64_t timestamp = 0;
    std::vector<unsigned char> frame_i420;  // from FramePool; EncodeWorker returns it
    int frame_length = 0;
    std::vector<GAI_Roi> rois_flat;         // rois_count * node_count points
    int rois_count = 0;
    int node_count = 0;
    std::vector<int> class_counts;          // kSupportedClasses order; empty => none
    int port = 0;                           // routing key (channel_id == port)
};

// Result envelope, serialised to the recorder's analytics JSON by SendWorker.
struct Envelope {
    int port_num = 0;
    std::vector<std::pair<std::string, std::string>> items;  // name -> value; empty => omitted
    std::vector<unsigned char> keyframe;                      // JPEG bytes (base64 on the wire)
    std::uint64_t timestamp = 0;
    std::vector<GAI_Roi> rois_flat;
    int rois_count = 0;
    int node_count = 0;
};

// Per-channel container (ChannelHandle.cs). Holds no worker threads: main owns
// the process-wide EncodeWorker / SendWorker pools that drain every channel.
class ChannelHandle {
public:
    // cap=5: stop-stream-to-callback-stop latency (residual buffer drain)
    // matters more than backpressure absorption.
    static constexpr std::size_t kEncodeQueueCapacity = 5;
    static constexpr std::size_t kSendQueueCapacity = 5;

    ChannelHandle(int port, const std::string& host);
    ~ChannelHandle();

    int Port() const { return port_; }
    ParameterStore& Parameters() { return params_; }
    BlockingQueue<RawDetection>& EncodeQ() { return encode_q_; }
    BlockingQueue<Envelope>& SendQ() { return send_q_; }

    bool StartListener();
    void StopListener();

    // ---- trigger-interval throttle (min interval between sends) ----------
    // True (and stamps "now") when at least interval_sec has elapsed since the
    // last pass; false to suppress. interval_sec <= 0 always passes. A CAS lets
    // exactly one frame per window win across the encode pool.
    bool TryPassTriggerInterval(int interval_sec);

    // ---- send-retry parking (per-channel failure isolation) --------------
    // A payload whose send failed is parked here instead of being retried in
    // place. While anything is parked the channel is "blocked": SendWorkers
    // take no new envelopes from its SendQ, so the failure's backpressure stays
    // on this channel instead of wedging the shared worker pool.
    bool HasParkedSend() const;
    void ParkSend(std::string payload, std::chrono::milliseconds backoff);
    bool TryClaimParkedSend(std::string& payload);
    void CompleteParkedSend(std::string payload, bool success, std::chrono::milliseconds backoff);

private:
    const int port_;
    ParameterStore params_;
    BlockingQueue<RawDetection> encode_q_{kEncodeQueueCapacity};
    BlockingQueue<Envelope> send_q_{kSendQueueCapacity};
    std::unique_ptr<HttpControlServer> listener_;

    std::atomic<std::int64_t> last_trigger_ns_{0};  // steady clock; 0 = never sent

    mutable std::mutex park_mtx_;
    std::deque<std::string> parked_;
    std::chrono::steady_clock::time_point retry_at_{};
    std::atomic<bool> retry_claimed_{false};
};

}  // namespace gai_host
