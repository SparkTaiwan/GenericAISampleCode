#pragma once

#include "gai_abi.h"
#include "idetector.h"
#include "roi_geometry.h"

#include <mutex>
#include <vector>

namespace gai {

// Stores the ROI rectangles + original polygon points coming in via
// GAI_SetParameters. The detect thread takes a copy each iteration; the HTTP
// listener thread updates under the same mutex. This mutex never touches the
// detection mutex, so SetParameters cannot stall behind a long Detect() call.
class ParamSnapshot {
public:
    struct View {
        std::vector<ROIRect> roi_rects;
        std::vector<std::vector<GAI_Roi>> original_roi_points;
        DetectorParams params;
    };

    void Apply(const GAI_Settings& settings);
    // Per-channel ai_settings (dynamic schema values, spec §5). Stored separately
    // from latest_params_ so a later Apply(GAI_Settings) does not clobber it; merged
    // into the View in Take(). confidence: 0..1, or <0 to clear. class_mask: bitmask
    // over class_table.h supported index, or <0 for "all supported". sensitivity /
    // threshold: 0..100 motion tunables that override the per-ROI values, or <0 to
    // leave the per-ROI value in place.
    // min/max_object_size: object detection bounding-box area band as % of frame (0..100);
    // min 0 = no lower limit, max >=100 = no upper limit; <0 to leave unset.
    void ApplyAiSettings(float confidence, int class_mask, int sensitivity, int threshold,
                         float min_object_size, float max_object_size);
    View Take() const;

private:
    mutable std::mutex mtx_;
    std::vector<ROIRect> roi_rects_;
    std::vector<std::vector<GAI_Roi>> original_roi_points_;
    // Default mirrors the legacy MotionDetector ctor (threshold=25, sensitivity=50)
    // so a SetParameters call carrying ROIs but no enabled sensitivity does not
    // collapse Motion mode to "any pixel diff fires" (pixel_threshold=0).
    DetectorParams latest_params_ = []{
        DetectorParams p; p.threshold = 25; p.sensitivity = 50; return p;
    }();
    // Per-channel ai_settings (kept apart from latest_params_ which Apply()
    // rebuilds). -1 = unset.
    float ai_confidence_  = -1.0f;
    int   ai_class_mask_  = -1;
    int   ai_sensitivity_ = -1;   // motion: overrides per-ROI sensitivity when >= 0
    int   ai_threshold_   = -1;   // motion: overrides per-ROI threshold when >= 0
    float ai_min_object_size_ = -1.0f;   // object detection: min box area as % of frame (0..100)
    float ai_max_object_size_ = -1.0f;   // object detection: max box area as % of frame (0..100)
};

}  // namespace gai
