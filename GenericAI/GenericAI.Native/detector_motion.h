#pragma once

#include "idetector.h"
#include "roi_geometry.h"

#include <cstdint>
#include <memory>
#include <vector>

// Per-channel state for MotionDetector. previous_frame is genuine cross-frame
// state (the diff needs the previous Y plane). frame_diff / thresh are work
// buffers parked here to avoid a per-Detect heap alloc — they only resize when
// width/height change.
struct MotionDetectorContext : public gai::DetectorContext {
    std::vector<unsigned char> previous_frame;
    int previous_width = 0;
    int previous_height = 0;
    std::vector<unsigned char> frame_diff;
    std::vector<unsigned char> thresh;
};

// Motion detector using Y-channel frame differencing. Stateless w.r.t.
// detector itself; per-channel state lives in MotionDetectorContext, per-call
// tunables come in via DetectorParams.
class MotionDetector {
public:
    MotionDetector();
    ~MotionDetector();

    std::unique_ptr<gai::DetectorContext> CreateContext();

    int Detect(MotionDetectorContext& ctx,
               const unsigned char* yuv420_frame, int width, int height,
               const ROIRect* roi_rects, int roi_count,
               std::vector<int>& detected_roi_indices,
               const gai::DetectorParams& params);
};
