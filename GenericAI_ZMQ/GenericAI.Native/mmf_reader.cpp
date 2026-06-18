#include "pch.h"
#include "mmf_reader.h"
#include "gai_config.h"
#include "host_log.h"
#include "timing_recorder.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

namespace gai {

namespace {

constexpr std::int64_t kMmfHeader = 0x1234;
constexpr std::int64_t kMmfFooter = 0x4321;
constexpr std::size_t  kMmfFrameCapacity = 1920ull * 1080ull * 3ull;

#pragma pack(push, 8)
struct MmfData {
    std::int64_t header;
    int          image_status;       // 0=idle, 1=new frame, 2=consumed
    int          image_width;
    int          image_height;
    int          image_size;
    std::uint64_t timestamp;
    unsigned char image_data[kMmfFrameCapacity];
    std::int64_t footer;
};
#pragma pack(pop)

}  // namespace

MmfReader::MmfReader(int port, BufferPool& pool, BoundedQueue<FrameSlot*>& out)
    : port_(port), pool_(pool), out_(out) {}

MmfReader::~MmfReader() {
    RequestStop();
    Join();
    Unmap();
}

bool MmfReader::Start() {
    if (running_.exchange(true)) return false;
    thread_ = std::thread(&MmfReader::Run, this);
    return true;
}

void MmfReader::RequestStop() {
    running_.store(false, std::memory_order_release);
}

void MmfReader::Join() {
    if (thread_.joinable()) thread_.join();
}

bool MmfReader::EnsureMapped() {
    if (mapped_view_ != nullptr) return true;

    if (map_handle_ == INVALID_HANDLE_VALUE || map_handle_ == nullptr) {
        char name[64];
        std::snprintf(name, sizeof(name), "ChannelFrame_%d", port_);
        map_handle_ = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, name);
        if (map_handle_ == nullptr) return false;
    }

    mapped_view_ = MapViewOfFile(map_handle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(MmfData));
    if (mapped_view_ == nullptr) {
        CloseHandle(map_handle_);
        map_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    MmfData* data = static_cast<MmfData*>(mapped_view_);
    if (data->header != kMmfHeader || data->footer != kMmfFooter) {
        std::memset(data, 0, sizeof(MmfData));
        data->header = kMmfHeader;
        data->footer = kMmfFooter;
    }

    // Once per successful map: the absence of this line in the host log means
    // the writer side never created ChannelFrame_<port> (or its mapping is
    // smaller than MmfData), i.e. no frame can ever reach detection.
    HostLog(HostLogLevel::Info,
            "[channel " + std::to_string(port_) + "] MMF ChannelFrame_" +
            std::to_string(port_) + " mapped");
    return true;
}

void MmfReader::Unmap() {
    if (mapped_view_) {
        UnmapViewOfFile(mapped_view_);
        mapped_view_ = nullptr;
    }
    if (map_handle_ != INVALID_HANDLE_VALUE && map_handle_ != nullptr) {
        CloseHandle(map_handle_);
        map_handle_ = INVALID_HANDLE_VALUE;
    }
}

void MmfReader::Run() {
    using namespace std::chrono;

    // Throttle for the oversized-frame warning below: dropping is silent and
    // per-frame, so without this a resolution mismatch floods the console.
    int warned_size = 0;
    // Throttle for the incoming-resolution info line: once per (w, h) change.
    int logged_w = 0;
    int logged_h = 0;

    while (running_.load(std::memory_order_acquire)) {
        if (!EnsureMapped()) {
            std::this_thread::sleep_for(milliseconds(50));
            continue;
        }

        MmfData* data = static_cast<MmfData*>(mapped_view_);
        if (data->image_status != 1) {
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }

        const int size = data->image_size;
        // The recorder allocates its decode buffer with 32-byte-aligned plane
        // strides but fills it packed (av_image_fill_arrays with align=1), so
        // image_size over-reports the actual I420 payload by the alignment
        // slack whenever width/2 is not a multiple of 32 (e.g. 352x240 reports
        // 130560 bytes for a 126720-byte image). Validate and copy the packed
        // size computed from the frame's own dimensions; image_size only has
        // to cover it.
        const std::size_t payload =
            (data->image_width > 0 && data->image_height > 0)
                ? static_cast<std::size_t>(data->image_width) *
                      data->image_height * 3 / 2
                : 0;
        // The actual incoming resolution is the first thing to compare against
        // the /SetParameters one when detection goes quiet, so it goes to the
        // host log on the first frame and on every change — for dropped
        // oversized frames too (the drop happens right below).
        if ((data->image_width != logged_w || data->image_height != logged_h) &&
            data->image_width > 0 && data->image_height > 0) {
            logged_w = data->image_width;
            logged_h = data->image_height;
            HostLog(HostLogLevel::Info,
                    "[channel " + std::to_string(port_) + "] incoming frames " +
                    std::to_string(logged_w) + "x" + std::to_string(logged_h) +
                    " (" + std::to_string(size) + " bytes), pool slot capacity " +
                    std::to_string(pool_.SlotCapacity()) + " bytes");
        }

        // Two upper bounds: the pool slot capacity (sized to the stream
        // resolution) guards the memcpy destination, and kMmfFrameCapacity
        // guards the source — when the configured resolution makes slots
        // bigger than the fixed MMF image_data area (e.g. 4K), corrupt
        // dimensions would otherwise read past the mapped view. image_size
        // must at least cover the packed payload, or the tail of the copy
        // would be stale bytes from the previous frame.
        if (size <= 0 || payload == 0 ||
            static_cast<std::size_t>(size) < payload ||
            payload > kMmfFrameCapacity ||
            payload > pool_.SlotCapacity()) {
            // Corrupt / oversized frame — ack so recorder doesn't stall, keep going.
            // Every such frame is dropped before detection, so warn loudly (once
            // per distinct size): the usual cause is a stream resolution larger
            // than the first /SetParameters sized the pool for.
            if (payload > pool_.SlotCapacity() && size != warned_size) {
                warned_size = size;
                const std::string msg =
                    "[channel " + std::to_string(port_) + "] dropping frames: " +
                    std::to_string(data->image_width) + "x" +
                    std::to_string(data->image_height) +
                    " (" + std::to_string(payload) + " bytes packed) exceeds the pool slot of " +
                    std::to_string(pool_.SlotCapacity()) +
                    " bytes sized from the first /SetParameters; "
                    "restart the channel to change resolution.";
                std::cerr << msg << std::endl;
                HostLog(HostLogLevel::Warn, msg);
            }
            data->image_status = 2;
            continue;
        }

        FrameSlot* slot = pool_.Acquire();
        if (slot == nullptr) {
            // Pool full: do NOT ack (leave status=1). Wrapper never drops
            // frames on its own — the recorder side sees status not flipped
            // and applies its own backpressure (skipping or overwriting its
            // next frame). Sleep briefly and retry; do not emit a timing
            // record (this frame has not entered our processing pipeline yet).
            pool_stall_count_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }

        std::memcpy(slot->data.data(), data->image_data, payload);
        slot->width = data->image_width;
        slot->height = data->image_height;
        slot->size = static_cast<int>(payload);
        slot->timestamp = data->timestamp;

        if (kEnableTimingLog) TimingRecorder::Instance().MarkRead(slot->timestamp, port_);

        data->image_status = 2;
        frames_read_.fetch_add(1, std::memory_order_relaxed);

        // Spin-with-sleep instead of dropping: keep retrying until the
        // slot is accepted, or shutdown. Wrapper never drops frames on its
        // own — pressure backs up through pool exhaustion (Acquire above)
        // to share memory. TryPushRef leaves `slot` intact on failure so
        // we can retry the same value.
        while (running_.load(std::memory_order_acquire)) {
            if (out_.TryPushRef(slot)) {
                if (kEnableTimingLog) TimingRecorder::Instance().MarkInferQueueIn(slot->timestamp);
                slot = nullptr;
                break;
            }
            pool_stall_count_.fetch_add(1, std::memory_order_relaxed);
            std::this_thread::sleep_for(milliseconds(1));
        }
        if (slot != nullptr) {
            // Shutdown while still holding the slot — release to avoid leak.
            pool_.Release(slot);
        }
    }

    Unmap();
}

}  // namespace gai
