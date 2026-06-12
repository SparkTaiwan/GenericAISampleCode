#include "pch.h"
#include "detector_motion.h"

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

using namespace std;

namespace {

// 1 = no subsample (full resolution); raise to 2+ to trade detection
// granularity for throughput on multi-channel 1080p deployments.
constexpr int kSubSample = 1;
constexpr int subsample_factor = kSubSample * kSubSample;

}  // namespace

MotionDetector::MotionDetector() {
    cout << "[MotionDetector] Initialized (stateless; per-call params)" << endl;
}

MotionDetector::~MotionDetector() = default;

int MotionDetector::Detect(MotionDetectorContext& ctx,
                           const unsigned char* yuv420_frame, int width, int height,
                           const ROIRect* roi_rects, int roi_count,
                           std::vector<int>& detected_roi_indices,
                           const DetectorParams& params)
{
    detected_roi_indices.clear();

    try {
        if (roi_rects == nullptr || roi_count <= 0) {
            return 0;
        }

        // Per-call tunables:
        //   pixel_threshold = 8 + (threshold/100) * 32      -> 8..40 (non-zero floor
        //     so threshold=0 doesn't make every ±1 compression artifact register)
        //   min_ratio       = 0.20 - (sensitivity/100) * 0.195  -> 0.005..0.20 of
        //     each ROI's own area, so the same sensitivity behaves consistently on
        //     a 320x240 ROI and a 1920x1080 ROI (absolute pixel-count thresholds
        //     do not).
        const int t = max(0, min(100, params.threshold));
        const int sensitivity = max(0, min(100, params.sensitivity));
        const int pixel_threshold = 8 + static_cast<int>((t / 100.0) * 32.0);
        const float min_ratio = 0.20f - (sensitivity / 100.0f) * 0.195f;

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

            const int roi_x1 = max(0, min(roi.x1, roi.x2));
            const int roi_x2 = min(width, max(roi.x1, roi.x2));
            const int roi_y1 = max(0, min(roi.y1, roi.y2));
            const int roi_y2 = min(height, max(roi.y1, roi.y2));

            const int roi_area = (roi_x2 - roi_x1) * (roi_y2 - roi_y1);
            const int effective_min_area = max(1,
                static_cast<int>(roi_area * min_ratio) / subsample_factor);

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

                cout << "[MotionDetector] Motion detected in ROI[" << roi_idx << "]: ("
                     << roi_x1 << ", " << roi_y1 << ") to (" << roi_x2 << ", " << roi_y2
                     << ") - >=" << motion_pixels << " changed pixels" << endl;
            }
        }

        // Update previous frame for next call.
        std::memcpy(ctx.previous_frame.data(), current_gray, y_size);

        if (detected_roi_indices.size() > 0) {
            cout << "[MotionDetector] Total: " << detected_roi_indices.size() << " ROI(s) with motion" << endl;
        }

        return static_cast<int>(detected_roi_indices.size());

    } catch (const exception& e) {
        cout << "[MotionDetector] Detection error: " << e.what() << endl;
        return 0;
    }
}
