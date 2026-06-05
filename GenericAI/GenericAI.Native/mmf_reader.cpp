#include "pch.h"
#include "mmf_reader.h"
#include "timing_recorder.h"

#include <chrono>
#include <cstdio>
#include <cstring>

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
        // Upper bound is the pool slot capacity (sized to the stream resolution),
        // not the legacy MMF struct size — a frame larger than a slot would
        // overflow the memcpy below, so drop it.
        if (size <= 0 || static_cast<std::size_t>(size) > pool_.SlotCapacity()) {
            // Corrupt / oversized frame — ack so recorder doesn't stall, keep going.
            data->image_status = 2;
            continue;
        }

        FrameSlot* slot = pool_.Acquire();
        if (slot == nullptr) {
            // Pool full: do NOT ack (leave status=1). Wrapper never drops
            // frames on its own — the recorder side sees status not flipped
            // and applies its own backpressure (skipping or overwriting its
            // next frame). Sleep briefly and retry; do not count as drop and
            // do not emit a timing record (this frame has not entered our
            // processing pipeline yet).
            std::this_thread::sleep_for(milliseconds(1));
            continue;
        }

        std::memcpy(slot->data.data(), data->image_data, static_cast<std::size_t>(size));
        slot->width = data->image_width;
        slot->height = data->image_height;
        slot->size = size;
        slot->timestamp = data->timestamp;

        TimingRecorder::Instance().MarkRead(slot->timestamp, port_);

        data->image_status = 2;
        frames_read_.fetch_add(1, std::memory_order_relaxed);

        if (!out_.TryPush(slot)) {
            // Infer queue full — return slot to pool and count as drop.
            TimingRecorder::Instance().Flush(slot->timestamp, TimingRecorder::FrameState::DroppedInferQFull);
            pool_.Release(slot);
            frames_dropped_.fetch_add(1, std::memory_order_relaxed);
        } else {
            TimingRecorder::Instance().MarkInferQueueIn(slot->timestamp);
        }
    }

    Unmap();
}

}  // namespace gai
