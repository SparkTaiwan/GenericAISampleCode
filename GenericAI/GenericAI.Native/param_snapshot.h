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
};

}  // namespace gai
