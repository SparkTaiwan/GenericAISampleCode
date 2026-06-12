#include "pch.h"
#include "param_snapshot.h"

namespace gai {

void ParamSnapshot::Apply(const GAI_Settings& s) {
    std::vector<ROIRect> rects;
    std::vector<std::vector<GAI_Roi>> polygons;
    rects.reserve(10);
    polygons.reserve(10);

    DetectorParams params{};
    bool have_first = false;

    for (int i = 0; i < 10; ++i) {
        std::vector<GAI_Roi> pts;
        for (int j = 0; j < 10; ++j) {
            if (s.rois[i][j].x >= 0) {
                pts.push_back(s.rois[i][j]);
            }
        }
        if (pts.size() >= 3) {
            ROIRect r;
            r.x1 = pts[0].x;
            r.y1 = pts[0].y;
            r.x2 = pts[2].x;
            r.y2 = pts[2].y;
            rects.push_back(r);
            polygons.push_back(std::move(pts));

            // Only ROI slots that contributed a valid polygon to roi_rects are
            // allowed to source the (threshold, sensitivity) — otherwise we may
            // pick up tuning from an i whose ROI was dropped, mis-aligning the
            // params with the rects index the detector actually sees.
            if (!have_first) {
                params.threshold   = s.threshold[i];
                params.sensitivity = s.sensitivity[i];
                have_first         = true;
            }
        }
    }

    std::lock_guard<std::mutex> lk(mtx_);
    roi_rects_.swap(rects);
    original_roi_points_.swap(polygons);
    if (have_first) {
        latest_params_.threshold   = params.threshold;
        latest_params_.sensitivity = params.sensitivity;
    }
    // The reference resolution updates on every call that carries one, even
    // when no ROI group is valid — it describes the coordinate space of the
    // payload itself, not the tuning values (which keep their last good set).
    if (s.image_width > 0 && s.image_height > 0) {
        latest_params_.image_width  = s.image_width;
        latest_params_.image_height = s.image_height;
    }
}

ParamSnapshot::View ParamSnapshot::Take() const {
    std::lock_guard<std::mutex> lk(mtx_);
    View v;
    v.roi_rects = roi_rects_;
    v.original_roi_points = original_roi_points_;
    v.params = latest_params_;
    return v;
}

}  // namespace gai
