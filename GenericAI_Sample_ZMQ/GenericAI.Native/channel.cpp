#include "pch.h"
#include "channel.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <iostream>

namespace gai {

std::atomic<GAI_DetectionCallback> g_callback{nullptr};

namespace {
std::atomic<bool> g_zmq_frame_mode{false};
}  // namespace

void SetZmqFrameMode(bool on) { g_zmq_frame_mode.store(on, std::memory_order_release); }
bool IsZmqFrameMode() { return g_zmq_frame_mode.load(std::memory_order_acquire); }

namespace {

constexpr std::int64_t kMmfHeader = 0x1234;
constexpr std::int64_t kMmfFooter = 0x4321;
constexpr std::size_t  kMmfFrameCapacity = 1920ull * 1080ull * 3ull;

// Fire the no-ROI fallback once every kFallbackInterval frames (~2 s at
// 30 fps) so the callback path can be demonstrated before any valid ROI
// polygon arrives.
constexpr int kFallbackInterval = 60;

// Layout matches the legacy MMF_Data written by spark.recorder (header 0x1234,
// footer 0x4321, status 0=idle 1=new 2=consumed) so the recorder side stays
// unchanged.
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

// Maps the view's ROI rects and original polygon points from the
// /SetParameters reference space onto the actual frame resolution, in place.
// The view is this iteration's private copy (ParamSnapshot::Take), so the
// stored snapshot keeps the reference-space coordinates for later frames.
// The callback therefore always reports frame-space pixels, aligned with the
// keyframe JPEG the host encodes from this same frame.
void ScaleViewToFrame(ParamSnapshot::View& view, int frame_w, int frame_h) {
    const int cfg_w = view.params.image_width;
    const int cfg_h = view.params.image_height;
    if (!RoiScaleNeeded(cfg_w, cfg_h, frame_w, frame_h)) return;
    for (auto& r : view.roi_rects) {
        ScaleRoiRectToFrame(r, cfg_w, cfg_h, frame_w, frame_h);
    }
    for (auto& poly : view.original_roi_points) {
        for (auto& pt : poly) {
            pt.x = ScaleCoordToFrame(pt.x, cfg_w, frame_w);
            pt.y = ScaleCoordToFrame(pt.y, cfg_h, frame_h);
        }
    }
}

}  // namespace

Channel::Channel(int port) : port_(port) {}

Channel::~Channel() {
    RequestStop();
    Join();
    Unmap();
}

void Channel::Start() {
    if (running_.exchange(true)) return;
    thread_ = std::thread(&Channel::Run, this);
}

void Channel::RequestStop() {
    running_.store(false, std::memory_order_release);
    zmq_cv_.notify_all();  // wake a worker blocked in RunZmq()
}

void Channel::Join() {
    if (thread_.joinable()) thread_.join();
}

void Channel::ApplyParameters(const GAI_Settings& s) {
    params_.Apply(s);
    has_params_.store(true, std::memory_order_release);

    ParamSnapshot::View v = params_.Take();
    std::cout << "[channel " << port_ << "] parameters applied: "
              << v.roi_rects.size() << " ROI(s), threshold="
              << v.params.threshold << ", sensitivity="
              << v.params.sensitivity << std::endl;
}

bool Channel::EnsureMapped() {
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

    // Once per successful map: the absence of this line means the writer side
    // never created ChannelFrame_<port> (or its mapping is smaller than
    // MmfData), i.e. no frame can ever reach detection.
    std::cout << "[channel " << port_ << "] MMF ChannelFrame_" << port_
              << " mapped" << std::endl;
    return true;
}

void Channel::Unmap() {
    if (mapped_view_) {
        UnmapViewOfFile(mapped_view_);
        mapped_view_ = nullptr;
    }
    if (map_handle_ != INVALID_HANDLE_VALUE && map_handle_ != nullptr) {
        CloseHandle(map_handle_);
        map_handle_ = INVALID_HANDLE_VALUE;
    }
}

void Channel::Run() {
    // GAI_StartZmqReceiver flips the global ZMQ flag right after channel init
    // (before any /SetParameters), so this selects the ZMQ source before the first
    // frame. RunMmf/RunZmq each return when the mode flips, so a late switch is
    // handled too.
    while (running_.load(std::memory_order_acquire)) {
        if (IsZmqFrameMode()) RunZmq();
        else                  RunMmf();
    }
}

void Channel::RunZmq() {
    using namespace std::chrono;
    while (running_.load(std::memory_order_acquire) && IsZmqFrameMode()) {
        int w, h, size;
        std::uint64_t ts;
        {
            std::unique_lock<std::mutex> lk(zmq_mtx_);
            zmq_cv_.wait_for(lk, milliseconds(100), [this] {
                return zmq_has_frame_ || !running_.load(std::memory_order_acquire);
            });
            if (!zmq_has_frame_) continue;  // timeout / shutdown wake
            // Copy the pending frame into the reused detection buffer under the
            // lock (cheap memcpy), then detect outside the lock so a slow
            // callback does not block the receiver thread's next SubmitDecodedFrame.
            if (frame_.size() < static_cast<std::size_t>(zmq_size_))
                frame_.resize(zmq_size_);
            std::memcpy(frame_.data(), zmq_pending_.data(), zmq_size_);
            w = zmq_w_; h = zmq_h_; size = zmq_size_; ts = zmq_ts_;
            zmq_has_frame_ = false;
        }
        // Detection waits for the first /SetParameters (same as the MMF path).
        if (!has_params_.load(std::memory_order_acquire)) continue;
        RunDetection(frame_.data(), w, h, size, ts);
    }
}

void Channel::SubmitDecodedFrame(const unsigned char* yuv_i420, int width, int height,
                                 int size, std::uint64_t timestamp) {
    if (!yuv_i420 || size <= 0) return;
    // Drop frames that arrive before /SetParameters so we don't buffer them.
    if (!has_params_.load(std::memory_order_acquire)) return;
    {
        std::lock_guard<std::mutex> lk(zmq_mtx_);
        if (zmq_pending_.size() < static_cast<std::size_t>(size))
            zmq_pending_.resize(size);
        std::memcpy(zmq_pending_.data(), yuv_i420, size);
        zmq_w_ = width; zmq_h_ = height; zmq_size_ = size; zmq_ts_ = timestamp;
        zmq_has_frame_ = true;  // overwrites an unconsumed frame (drop-old)
    }
    zmq_cv_.notify_one();
}

void Channel::RunMmf() {
    using namespace std::chrono;

    // Log the incoming resolution on the first frame and on every change —
    // it is the first thing to compare against the /SetParameters one when
    // detection goes quiet.
    int logged_w = 0;
    int logged_h = 0;

    while (running_.load(std::memory_order_acquire) && !IsZmqFrameMode()) {
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
        // strides but fills it packed, so image_size over-reports the actual
        // I420 payload by the alignment slack whenever width/2 is not a
        // multiple of 32 (e.g. 352x240 reports 130560 bytes for a 126720-byte
        // image). Validate and copy the packed size computed from the frame's
        // own dimensions; image_size only has to cover it.
        const std::size_t payload =
            (data->image_width > 0 && data->image_height > 0)
                ? static_cast<std::size_t>(data->image_width) *
                      data->image_height * 3 / 2
                : 0;
        if ((data->image_width != logged_w || data->image_height != logged_h) &&
            data->image_width > 0 && data->image_height > 0) {
            logged_w = data->image_width;
            logged_h = data->image_height;
            std::cout << "[channel " << port_ << "] incoming frames "
                      << logged_w << "x" << logged_h
                      << " (" << size << " bytes)" << std::endl;
        }

        if (size <= 0 || payload == 0 ||
            static_cast<std::size_t>(size) < payload ||
            payload > kMmfFrameCapacity) {
            // Corrupt frame — ack so the recorder doesn't stall, keep going.
            // Warn once per distinct size so a stuck writer doesn't flood the
            // console.
            if (size != warned_size_) {
                warned_size_ = size;
                std::cout << "[channel " << port_ << "] dropping frame: image_size="
                          << size << " for " << data->image_width << "x"
                          << data->image_height << " is invalid" << std::endl;
            }
            data->image_status = 2;
            continue;
        }

        if (frame_.size() < payload) frame_.resize(payload);
        std::memcpy(frame_.data(), data->image_data, payload);
        const int w = data->image_width;
        const int h = data->image_height;
        const std::uint64_t ts = data->timestamp;
        data->image_status = 2;

        // Detection waits for the first /SetParameters; frames keep getting
        // consumed above so the writer side never stalls.
        if (!has_params_.load(std::memory_order_acquire)) continue;

        RunDetection(frame_.data(), w, h, static_cast<int>(payload), ts);
    }

    Unmap();
}

void Channel::RunDetection(const unsigned char* frame, int width, int height,
                           int size, std::uint64_t timestamp) {
    GAI_DetectionCallback cb = g_callback.load(std::memory_order_acquire);

    ParamSnapshot::View view = params_.Take();
    ScaleViewToFrame(view, width, height);

    if (view.roi_rects.empty()) {
        // Configured but no valid ROI polygon yet: demonstrate the callback
        // path anyway with two fixed quads.
        if (++fallback_counter_ % kFallbackInterval != 0) return;
        if (!cb) return;
        GAI_Roi flat[8] = {
            {0, 0},   {10, 10}, {30, 30}, {40, 40},
            {50, 50}, {60, 60}, {70, 70}, {80, 80},
        };
        cb(port_, width, height, frame, size, timestamp, flat, 2, 4);
        return;
    }

    std::vector<int> indices;
    int n = 0;
    try {
        n = detector_.Detect(detector_ctx_, frame, width, height,
                             view.roi_rects.data(),
                             static_cast<int>(view.roi_rects.size()),
                             indices, view.params);
    } catch (...) {
        // A detector exception must not take the channel down.
        return;
    }
    if (n <= 0 || !cb) return;

    // Flatten the original polygon points of the detected ROIs; node_count
    // assumes equal-size polygons (same contract as the production wrapper).
    std::vector<GAI_Roi> flat;
    for (int idx : indices) {
        if (idx >= 0 && static_cast<std::size_t>(idx) < view.original_roi_points.size()) {
            for (const auto& pt : view.original_roi_points[idx]) {
                flat.push_back(pt);
            }
        }
    }
    const int node_count = (n > 0) ? static_cast<int>(flat.size()) / n : 0;
    cb(port_, width, height, frame, size, timestamp, flat.data(), n, node_count);
}

}  // namespace gai
