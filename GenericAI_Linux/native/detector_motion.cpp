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

// Motion is judged on kBlock x kBlock block means, not single pixels. Sensor
// grain and the noise pattern that changes with every I-frame are high-
// frequency and cancel out in the mean; a moving object shifts the brightness
// of whole blocks and survives. Per-pixel diffs on a dark, grainy scene made
// >5% of a full-frame ROI "change" on every I-frame (a trigger every GOP even
// at sensitivity ~10, threshold 90).
constexpr int kBlock = 8;

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
        //     so threshold=0 doesn't make every ±1 compression artifact register),
        //     compared against the change of a block's MEAN luminance (see kBlock)
        //   min_ratio       = 0.10 * 0.005^(sensitivity/100)  -> 10%..0.05% of
        //     each ROI's own area, so the same sensitivity behaves consistently on
        //     a 320x240 ROI and a 1920x1080 ROI (absolute pixel-count thresholds
        //     do not). Log scale: every +25 is ~3.8x more sensitive, so the whole
        //     slider range is usable. On a full-frame 1080p ROI this needs ~14.7k
        //     changed pixels at 50, ~3.9k at 75, ~1k at 100. The former linear
        //     10%..0.5% mapping needed ~109k at 50 and missed real motion; a
        //     10%..0.005% floor (~104 px at 100) triggered on noise.
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
            const float min_ratio = 0.10f * std::pow(0.005f, sensitivity / 100.0f);

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

            const int effective_min_area = max(1, static_cast<int>(region_area * min_ratio));

            // Block scan over the ROI bbox: a block counts as changed when its mean
            // luminance moved by more than pixel_threshold, and then contributes all
            // of its pixels to motion_pixels, so min_ratio keeps meaning "fraction of
            // the ROI area". Edge blocks are clipped to the bbox. Early-exit once
            // motion_pixels reaches effective_min_area so a multi-channel deployment
            // doesn't pay for a full-ROI scan on every triggered frame; motion_pixels
            // at the log point therefore reflects the trigger threshold, not the full
            // count — logged with ">=".
            const unsigned char* previous_gray = ctx.previous_frame.data();
            int motion_pixels = 0;
            bool roi_done = false;
            for (int by = roi_y1; by < roi_y2 && !roi_done; by += kBlock) {
                const int bh = min(kBlock, roi_y2 - by);
                for (int bx = roi_x1; bx < roi_x2; bx += kBlock) {
                    const int bw = min(kBlock, roi_x2 - bx);
                    int sum_curr = 0, sum_prev = 0;
                    for (int y = by; y < by + bh; ++y) {
                        const unsigned char* curr_row = current_gray + y * width + bx;
                        const unsigned char* prev_row = previous_gray + y * width + bx;
                        for (int x = 0; x < bw; ++x) {
                            sum_curr += curr_row[x];
                            sum_prev += prev_row[x];
                        }
                    }
                    const int block_pixels = bw * bh;
                    // |mean_curr - mean_prev| > pixel_threshold, without dividing.
                    if (std::abs(sum_curr - sum_prev) <= pixel_threshold * block_pixels) continue;
                    // Only pay the polygon test for blocks that already changed; the
                    // block belongs to the ROI when its centre does.
                    if (poly != nullptr && !PointInPolygon(bx + bw / 2, by + bh / 2, *poly)) continue;
                    motion_pixels += block_pixels;
                    if (motion_pixels >= effective_min_area) {
                        roi_done = true;
                        break;
                    }
                }
            }

            if (motion_pixels >= effective_min_area) {
                detected_roi_indices.push_back(roi_idx);

                if (gai::VerboseLogging()) {
                    cout << "[MotionDetector] Motion detected in ROI[" << roi_idx << "] ("
                         << (poly != nullptr ? "polygon" : "rect") << ", bbox "
                         << roi_x1 << "," << roi_y1 << " to " << roi_x2 << "," << roi_y2
                         << ") - >=" << motion_pixels << " changed pixels (" << kBlock << "x" << kBlock
                         << " block means)" << endl;
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
