#include "pch.h"
#include "detector_motion.h"
#include "host_log.h"

#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <climits>
#include <vector>

using namespace std;

namespace {

// 1 = no subsample (full resolution); raise to 2+ to trade detection
// granularity for throughput on multi-channel 1080p deployments.
constexpr int kSubSample = 1;
constexpr int subsample_factor = kSubSample * kSubSample;

// PointInPolygon / PolygonArea are shared helpers in roi_geometry.h.

}  // namespace

MotionDetector::MotionDetector() {
    cout << "[MotionDetector] Initialized (stateless; per-call params)" << endl;
}

MotionDetector::~MotionDetector() = default;

std::unique_ptr<gai::DetectorContext> MotionDetector::CreateContext() {
    return std::unique_ptr<gai::DetectorContext>(new MotionDetectorContext());
}

int MotionDetector::Detect(MotionDetectorContext& ctx,
                           const unsigned char* yuv420_frame, int width, int height,
                           const ROIRect* roi_rects, int roi_count,
                           const std::vector<std::vector<GAI_Roi>>& original_roi_points,
                           std::vector<int>& detected_roi_indices,
                           const gai::DetectorParams& params)
{
    detected_roi_indices.clear();

    // Motion tuning is per-ROI now (rides on each ROIRect); the channel-wide
    // params.sensitivity/threshold are no longer read here.
    (void)params;

    try {
        if (roi_rects == nullptr || roi_count <= 0) {
            return 0;
        }

        // Tunables are PER-ROI (schema scope=roi): each ROIRect carries its own
        // sensitivity/threshold, so the pixel threshold + min-area ratio are computed
        // inside the ROI loop below rather than once for the whole channel. Formulae:
        //   pixel_threshold = 8 + (threshold/100) * 12      -> 8..20 (non-zero floor
        //     so threshold=0 doesn't make every ±1 compression artifact register)
        //   min_ratio       = 0.10 - (sensitivity/100) * 0.095  -> 0.005..0.10 of
        //     each ROI's own area, so the same sensitivity behaves consistently on
        //     a 320x240 ROI and a 1920x1080 ROI (absolute pixel-count thresholds
        //     do not).
        const int y_size = width * height;
        if (y_size <= 0) return 0;
        const unsigned char* current_gray = yuv420_frame;

        // First frame or resolution change → seed previous_frame, drop this frame.
        if (ctx.previous_frame.empty() ||
            ctx.previous_width != width || ctx.previous_height != height) {
            ctx.previous_frame.assign(current_gray, current_gray + y_size);
            ctx.previous_width = width;
            ctx.previous_height = height;
            return 0;
        }

        for (int roi_idx = 0; roi_idx < roi_count; roi_idx++) {
            const ROIRect& roi = roi_rects[roi_idx];

            // Per-ROI tuning: this region's own sensitivity/threshold.
            const int t = max(0, min(100, roi.threshold));
            const int sensitivity = max(0, min(100, roi.sensitivity));
            const int pixel_threshold = 8 + static_cast<int>((t / 100.0) * 12.0);
            const float min_ratio = 0.10f - (sensitivity / 100.0f) * 0.095f;

            // Prefer the ACTUAL polygon (Argo sends up to 10 points) when present;
            // fall back to the rect for legacy/degenerate ROIs (<3 points). The rect
            // in roi_rects is only pts[0]/pts[2], so for a real polygon we recompute
            // the true bounding box from all vertices for the scan bounds.
            const std::vector<GAI_Roi>* poly = nullptr;
            if (static_cast<size_t>(roi_idx) < original_roi_points.size() &&
                original_roi_points[roi_idx].size() >= 3) {
                poly = &original_roi_points[roi_idx];
            }

            int roi_x1, roi_y1, roi_x2, roi_y2;
            double region_area;
            if (poly != nullptr) {
                int minx = INT_MAX, miny = INT_MAX, maxx = INT_MIN, maxy = INT_MIN;
                for (const auto& p : *poly) {
                    minx = min(minx, p.x); maxx = max(maxx, p.x);
                    miny = min(miny, p.y); maxy = max(maxy, p.y);
                }
                roi_x1 = max(0, minx); roi_x2 = min(width, maxx);
                roi_y1 = max(0, miny); roi_y2 = min(height, maxy);
                region_area = PolygonArea(*poly);   // real area, not bbox
            } else {
                roi_x1 = max(0, min(roi.x1, roi.x2));
                roi_x2 = min(width, max(roi.x1, roi.x2));
                roi_y1 = max(0, min(roi.y1, roi.y2));
                roi_y2 = min(height, max(roi.y1, roi.y2));
                region_area = static_cast<double>((roi_x2 - roi_x1) * (roi_y2 - roi_y1));
            }

            const int effective_min_area = max(1,
                static_cast<int>(region_area * min_ratio) / subsample_factor);

            // Fused per-ROI scan: diff + threshold + count in one pass, with
            // early-exit once motion_pixels reaches effective_min_area so a
            // multi-channel deployment doesn't pay for a full-ROI scan on every
            // triggered frame. motion_pixels at the log point therefore reflects
            // the trigger threshold, not the full count — logged with ">=".
            int motion_pixels = 0;
            bool roi_done = false;
            for (int y = roi_y1; y < roi_y2 && !roi_done; y += kSubSample) {
                const unsigned char* curr_row = current_gray + y * width;
                const unsigned char* prev_row = ctx.previous_frame.data() + y * width;
                for (int x = roi_x1; x < roi_x2; x += kSubSample) {
                    const int diff = std::abs(
                        static_cast<int>(curr_row[x]) - static_cast<int>(prev_row[x]));
                    if (diff > pixel_threshold) {
                        // Only pay the polygon test for pixels that already changed —
                        // motion pixels are sparse, so this keeps the cost close to the
                        // plain rect scan even for many-vertex ROIs.
                        if (poly != nullptr && !PointInPolygon(x, y, *poly)) continue;
                        ++motion_pixels;
                        if (motion_pixels >= effective_min_area) {
                            roi_done = true;
                            break;
                        }
                    }
                }
            }

            if (motion_pixels >= effective_min_area) {
                detected_roi_indices.push_back(roi_idx);

                if (gai::VerboseLogging()) {
                    cout << "[MotionDetector] Motion detected in ROI[" << roi_idx << "] ("
                         << (poly != nullptr ? "polygon" : "rect") << ", bbox "
                         << roi_x1 << "," << roi_y1 << " to " << roi_x2 << "," << roi_y2
                         << ") - >=" << motion_pixels << " changed pixels" << endl;
                }
            }
        }

        // Update previous frame for next call.
        std::memcpy(ctx.previous_frame.data(), current_gray, y_size);

        if (detected_roi_indices.size() > 0 && gai::VerboseLogging()) {
            cout << "[MotionDetector] Total: " << detected_roi_indices.size() << " ROI(s) with motion" << endl;
        }

        return static_cast<int>(detected_roi_indices.size());

    } catch (const exception& e) {
        cout << "[MotionDetector] Detection error: " << e.what() << endl;
        return 0;
    }
}
