#pragma once

#include "roi_geometry.h"

#include <vector>

// Per-call detector tunables, taken from the channel's latest /SetParameters
// snapshot. image_width/height carry the reference resolution the ROI
// coordinates were authored in; 0 means unknown and disables ROI rescaling.
struct DetectorParams {
    int threshold = 0;
    int sensitivity = 0;
    int image_width = 0;
    int image_height = 0;
};

// Per-channel state for MotionDetector. previous_frame is genuine cross-frame
// state (the diff needs the previous Y plane); the per-ROI fused loop computes
// diff/threshold/count in one pass so no full-frame work buffers are needed.
struct MotionDetectorContext {
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

    int Detect(MotionDetectorContext& ctx,
               const unsigned char* yuv420_frame, int width, int height,
               const ROIRect* roi_rects, int roi_count,
               std::vector<int>& detected_roi_indices,
               const DetectorParams& params);
};
