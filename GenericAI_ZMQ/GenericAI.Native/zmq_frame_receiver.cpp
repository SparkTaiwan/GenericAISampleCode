#include "pch.h"
#include "zmq_frame_receiver.h"
#include "zmq_frame_header.h"
#include "nal_decoder.h"
#include "channel_pipeline.h"
#include "shared_detector_scheduler.h"
#include "host_log.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <thread>

// Core 0MQ C API (NuGet: libzmq-vc143). We use the C API directly so cppzmq
// (zmq.hpp) is not required.
#include <zmq.h>

namespace gai {

struct ZmqFrameReceiver::Impl {
    std::string endpoint;
    SharedDetectorScheduler* scheduler = nullptr;

    void* ctx  = nullptr;  // zmq context
    void* sock = nullptr;  // ZMQ_PULL socket

    std::atomic<bool> running{false};
    std::thread thread;

    // One decoder per channel_id. Only touched by the receive thread -> no lock.
    std::map<uint32_t, std::unique_ptr<NalDecoder>> decoders;

    Impl(std::string ep, SharedDetectorScheduler* sched)
        : endpoint(std::move(ep)), scheduler(sched) {}

    NalDecoder* DecoderFor(uint32_t channel_id, uint8_t codec) {
        auto it = decoders.find(channel_id);
        if (it != decoders.end()) return it->second.get();
        auto dec = std::unique_ptr<NalDecoder>(new NalDecoder(codec));
        if (!dec->Valid()) {
            HostLog(HostLogLevel::Error,
                    "ZmqFrameReceiver: failed to build decoder for channel " +
                    std::to_string(channel_id));
        }
        NalDecoder* raw = dec.get();
        decoders.emplace(channel_id, std::move(dec));
        return raw;
    }

    bool HasMore() const {
        int more = 0;
        size_t sz = sizeof(more);
        if (zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &sz) != 0) return false;
        return more != 0;
    }

    // Discard any remaining parts of the current multipart message.
    void DrainExtraParts() {
        while (HasMore()) {
            zmq_msg_t junk;
            zmq_msg_init(&junk);
            int rc = zmq_msg_recv(&junk, sock, 0);
            zmq_msg_close(&junk);
            if (rc < 0) break;
        }
    }

    void Run() {
        const int rcvtimeo = 200;  // ms, lets the loop poll `running`
        // RCVHWM scales with channel_count to match the module's per-channel frame
        // slack; a fixed 8 would drop frames hard once a shard carries many channels.
        int chans = scheduler ? static_cast<int>(scheduler->ChannelCount()) : 1;
        if (chans < 1) chans = 1;
        int rcvhwm = chans * 4;
        if (rcvhwm < 8) rcvhwm = 8;
        const int linger   = 0;    // close/ctx_term must never block flushing
        zmq_setsockopt(sock, ZMQ_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
        zmq_setsockopt(sock, ZMQ_RCVHWM, &rcvhwm, sizeof(rcvhwm));
        zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));

        if (zmq_connect(sock, endpoint.c_str()) != 0) {
            HostLog(HostLogLevel::Error,
                    std::string("ZmqFrameReceiver: connect failed: ") + zmq_strerror(zmq_errno()));
            return;
        }
        HostLog(HostLogLevel::Info, "ZmqFrameReceiver: connected to " + endpoint
                + " (channels=" + std::to_string(chans)
                + " rcvhwm=" + std::to_string(rcvhwm) + ")");

        // Diagnostics: distinguishes "decode failing" (received climbs, decoded
        // stays 0 -> SPS/PPS missing) from "detector found nothing" (both climb
        // but no [DETECT]). Printed to the AI console.
        unsigned long long recvCount = 0;
        unsigned long long decodeCount = 0;

        while (running.load(std::memory_order_acquire)) {
            // Part 0: header.
            zmq_msg_t hdr_msg;
            zmq_msg_init(&hdr_msg);
            int n = zmq_msg_recv(&hdr_msg, sock, 0);
            if (n < 0) {
                zmq_msg_close(&hdr_msg);
                if (zmq_errno() == EAGAIN) continue;  // rcvtimeo elapsed
                break;                                 // ETERM / socket closed
            }

            bool ok = (zmq_msg_size(&hdr_msg) == sizeof(ZmqFrameHeader));
            ZmqFrameHeader hdr{};
            if (ok) {
                std::memcpy(&hdr, zmq_msg_data(&hdr_msg), sizeof(hdr));
                ok = (hdr.magic == kZmqFrameMagic && hdr.version == kZmqFrameVersion);
            }
            zmq_msg_close(&hdr_msg);
            if (!ok) { DrainExtraParts(); continue; }

            if (!HasMore()) continue;  // malformed: header with no payload part

            // Part 1: NAL payload.
            zmq_msg_t nal_msg;
            zmq_msg_init(&nal_msg);
            n = zmq_msg_recv(&nal_msg, sock, 0);
            if (n < 0) {
                zmq_msg_close(&nal_msg);
                if (zmq_errno() == EAGAIN) continue;
                break;
            }

            ChannelPipeline* ch = scheduler
                ? scheduler->FindByPort(static_cast<int>(hdr.channel_id)) : nullptr;
            NalDecoder* dec = ch ? DecoderFor(hdr.channel_id, hdr.codec) : nullptr;
            if (ch && dec && dec->Valid()) {
                ++recvCount;
                const std::uint64_t ts = hdr.timestamp;
                dec->Decode(static_cast<const unsigned char*>(zmq_msg_data(&nal_msg)),
                            static_cast<int>(zmq_msg_size(&nal_msg)),
                            [ch, ts, &decodeCount](const unsigned char* i420, int w, int h) {
                                ++decodeCount;
                                ch->SubmitDecodedFrame(i420, w, h, w * h * 3 / 2, ts);
                            });
                if ((recvCount % 30) == 0) {
                    std::cout << "[zmq] ch=" << ch->Port()
                              << " received=" << recvCount
                              << " decoded=" << decodeCount << std::endl;
                }
            }
            else if (!ch && (++recvCount % 30) == 0) {
                std::cout << "[zmq] WARN no channel for channel_id=" << hdr.channel_id
                          << " (check the 'channel=' arg matches a wrapper port)" << std::endl;
            }
            zmq_msg_close(&nal_msg);

            DrainExtraParts();  // tolerate >2-part messages defensively
        }
    }
};

ZmqFrameReceiver::ZmqFrameReceiver(std::string endpoint, SharedDetectorScheduler* scheduler)
    : impl_(new Impl(std::move(endpoint), scheduler)) {}

ZmqFrameReceiver::~ZmqFrameReceiver() { Stop(); }

bool ZmqFrameReceiver::Start() {
    if (!impl_) return false;
    if (impl_->running.load()) return true;  // already started

    impl_->ctx = zmq_ctx_new();
    if (!impl_->ctx) return false;

    // Scale I/O threads with the shard size (one PULL carries every channel),
    // ~1 per 16 channels, capped. Must be set before the socket is created.
    int chans = impl_->scheduler ? static_cast<int>(impl_->scheduler->ChannelCount()) : 1;
    if (chans < 1) chans = 1;
    int ioThreads = (chans + 15) / 16;
    if (ioThreads < 1) ioThreads = 1;
    if (ioThreads > 4) ioThreads = 4;
    zmq_ctx_set(impl_->ctx, ZMQ_IO_THREADS, ioThreads);

    impl_->sock = zmq_socket(impl_->ctx, ZMQ_PULL);
    if (!impl_->sock) {
        zmq_ctx_term(impl_->ctx);
        impl_->ctx = nullptr;
        return false;
    }
    impl_->running.store(true, std::memory_order_release);
    impl_->thread = std::thread(&Impl::Run, impl_.get());
    return true;
}

void ZmqFrameReceiver::Stop() {
    if (!impl_) return;
    impl_->running.store(false, std::memory_order_release);

    // zmq_ctx_shutdown makes any blocking call on this context (the Run thread's
    // recv) return immediately with ETERM -> Run exits at once, so join never
    // waits on a stuck recv. The Run thread owns the socket (not thread-safe), so
    // we still close it only AFTER the join. LINGER=0 keeps ctx_term from blocking.
    if (impl_->ctx) zmq_ctx_shutdown(impl_->ctx);
    if (impl_->thread.joinable()) impl_->thread.join();
    if (impl_->sock) { zmq_close(impl_->sock); impl_->sock = nullptr; }
    if (impl_->ctx)  { zmq_ctx_term(impl_->ctx); impl_->ctx = nullptr; }
}

}  // namespace gai
