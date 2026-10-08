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
            // Per-ROI tuning (schema scope=roi): the sensitivity/threshold travel
            // ON the rect, aligned by construction with the index the detector sees.
            // Motion reads roi.sensitivity/roi.threshold per ROI; no channel-wide
            // collapse. s.sensitivity[i]/s.threshold[i] come from rois[i] on the wire.
            r.sensitivity = s.sensitivity[i];
            r.threshold   = s.threshold[i];
            // Per-ROI object-detection tuning (schema scope=roi). -1 = unset ->
            // ApplyRoiFilter inherits the channel ai_settings value.
            r.confidence      = s.confidence[i];
            r.class_mask      = s.class_mask[i];
            r.object_size_min = s.object_size_min[i];
            r.object_size_max = s.object_size_max[i];
            rects.push_back(r);
            polygons.push_back(std::move(pts));

            // Keep the first valid ROI's tuning as latest_params_ fallback only
            // (whole-frame path / legacy callers). It is NOT the per-ROI source.
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

void ParamSnapshot::ApplyAiSettings(float confidence, int class_mask, int sensitivity, int threshold,
                                    float min_object_size, float max_object_size) {
    std::lock_guard<std::mutex> lk(mtx_);
    ai_confidence_  = confidence;
    ai_class_mask_  = class_mask;
    ai_sensitivity_ = sensitivity;
    ai_threshold_   = threshold;
    ai_min_object_size_ = min_object_size;
    ai_max_object_size_ = max_object_size;
}

ParamSnapshot::View ParamSnapshot::Take() const {
    std::lock_guard<std::mutex> lk(mtx_);
    View v;
    v.roi_rects = roi_rects_;
    v.original_roi_points = original_roi_points_;
    v.params = latest_params_;
    v.params.confidence = ai_confidence_;   // per-channel ai_settings override
    v.params.class_mask = ai_class_mask_;
    v.params.min_object_size = ai_min_object_size_;   // object detection size band (min)
    v.params.max_object_size = ai_max_object_size_;   // object detection size band (max)
    // Motion sensitivity/threshold are per-ROI now (schema scope=roi): they ride on
    // each ROIRect (set in Apply), so there is NO channel-wide override here — that
    // would flatten every region to one value. ai_sensitivity_/ai_threshold_ are
    // left in place but unused by motion (kept for ABI compatibility).
    return v;
}

}  // namespace gai
