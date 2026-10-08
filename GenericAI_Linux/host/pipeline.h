#pragma once

// Detection result pipeline, ported from GenericAI.App/Pipeline:
//   native callback -> FrameDispatcher -> ChannelHandle::EncodeQ
//   -> EncodeWorker (JPEG) -> ChannelHandle::SendQ
//   -> SendWorker (ZMQ PUSH or HTTP POST, park + retry on failure)

#include "channel_handle.h"
#include "state.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gai_host {

// Reuses multi-MB I420 buffers between detections (C# ArrayPool<byte>).
class FramePool {
public:
    static std::vector<unsigned char> Rent(std::size_t size);
    static void Return(std::vector<unsigned char>&& buf);
};

// Native detection callback target. Copies the frame and ROI/class data off
// the native buffers and queues it on the owning channel's EncodeQ; blocks
// there for backpressure (the wrapper never drops frames on its own).
class FrameDispatcher {
public:
    FrameDispatcher(const std::map<int, ChannelHandle*>& by_channel_id, DropCounter& drops);
    void Register();
    void Unregister();

private:
    static void OnNativeCallback(int channel_id, int width, int height,
                                 const unsigned char* frame_i420, int frame_size,
                                 unsigned long long timestamp,
                                 const GAI_Roi* rois_flat, int rois_count, int node_count,
                                 const int* class_counts, int class_counts_len);

    static FrameDispatcher* instance_;
    const std::map<int, ChannelHandle*>& by_channel_id_;
    DropCounter& drops_;
    bool registered_ = false;
};

// Shared stop flag for the worker pools (the C# CancellationToken).
class StopToken {
public:
    void Cancel() { stopped_.store(true); }
    bool IsCancelled() const { return stopped_.load(); }

private:
    std::atomic<bool> stopped_{false};
};

class EncodeWorker {
public:
    EncodeWorker(std::vector<ChannelHandle*> channels, DropCounter& drops, StopToken& stop);
    void Start();
    void Join();

private:
    void Run();

    std::vector<ChannelHandle*> channels_;
    DropCounter& drops_;
    StopToken& stop_;
    std::thread thread_;
};

#ifdef USE_ZMQ
// Analytics results over ZMQ (module BINDs a PULL, we CONNECT a PUSH). One
// process-wide socket for all channels; Send() is serialised because ZMQ
// sockets are not thread-safe.
class ZmqResultSender {
public:
    ZmqResultSender(const std::string& endpoint, int channel_count);  // throws on failure
    ~ZmqResultSender();
    bool Send(const std::string& payload);
    void Close();

private:
    std::mutex mtx_;
    std::string endpoint_;
    void* ctx_ = nullptr;
    void* sock_ = nullptr;
    bool closed_ = false;
};
#endif

class SendWorker {
public:
    SendWorker(std::vector<ChannelHandle*> channels, DropCounter& drops, StopToken& stop
#ifdef USE_ZMQ
               , ZmqResultSender* zmq
#endif
    );
    void Start();
    void Join();

private:
    void Run();
    bool TrySendOnce(const std::string& url, const std::string& payload, int port);
    bool HttpPost(const std::string& url, const std::string& payload, std::string& error);

    std::vector<ChannelHandle*> channels_;
    DropCounter& drops_;
    StopToken& stop_;
#ifdef USE_ZMQ
    ZmqResultSender* zmq_;  // null => HTTP POST path
#endif
    std::thread thread_;
};

// Envelope -> analytics JSON (byte-compatible field order with HttpEnvelope.cs).
std::string SerializeEnvelope(const Envelope& env);

}  // namespace gai_host
