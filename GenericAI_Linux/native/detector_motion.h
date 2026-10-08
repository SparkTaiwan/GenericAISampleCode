#pragma once

#include "idetector.h"
#include "roi_geometry.h"

#include <cstdint>
#include <memory>
#include <vector>

// Per-channel state for MotionDetector. previous_frame is genuine cross-frame
// state (the diff needs the previous Y plane); the per-ROI fused loop computes
// diff/threshold/count in one pass so no full-frame work buffers are needed.
struct MotionDetectorContext : public gai::DetectorContext {
    std::vector<unsigned char> previous_frame;
    int previous_width = 0;
    int previous_height = 0;
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
               const std::vector<std::vector<GAI_Roi>>& original_roi_points,
               std::vector<int>& detected_roi_indices,
               const gai::DetectorParams& params);
};
