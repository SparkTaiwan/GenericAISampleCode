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

void ThresholdBinary(const unsigned char* input, unsigned char* output,
                     int width, int height, int threshold) {
    const int size = width * height;
    for (int i = 0; i < size; i++) {
        output[i] = (input[i] > threshold) ? 255 : 0;
    }
}

void CalculateFrameDiff(const unsigned char* frame1, const unsigned char* frame2,
                        unsigned char* output, int width, int height) {
    const int size = width * height;
    for (int i = 0; i < size; i++) {
        const int diff = static_cast<int>(frame1[i]) - static_cast<int>(frame2[i]);
        output[i] = static_cast<unsigned char>(std::abs(diff));
    }
}

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
                           std::vector<int>& detected_roi_indices,
                           const gai::DetectorParams& params)
{
    detected_roi_indices.clear();

    try {
        if (roi_rects == nullptr || roi_count <= 0) {
            return 0;
        }

        // Per-call tunables (legacy mapping from SetThresholdAndSensitivity):
        //   pixel_threshold = (threshold/100) * 50
        //   sensitivity     = clamp(0..100)
        //   min_area        = 2000 - (sensitivity/100) * 1700
        const int sensitivity = max(0, min(100, params.sensitivity));
        int pixel_threshold = static_cast<int>((params.threshold / 100.0) * 50.0);
        if (pixel_threshold < 0) pixel_threshold = 0;
        const int min_area = static_cast<int>(2000 - (sensitivity / 100.0) * 1700);

        const int y_size = width * height;
        if (y_size <= 0) return 0;
        const unsigned char* current_gray = yuv420_frame;

        // First frame or resolution change → seed previous_frame, drop this frame.
        if (ctx.previous_frame.empty() ||
            ctx.previous_width != width || ctx.previous_height != height) {
            ctx.previous_frame.assign(current_gray, current_gray + y_size);
            ctx.previous_width = width;
            ctx.previous_height = height;
            ctx.frame_diff.clear();
            ctx.thresh.clear();
            return 0;
        }

        // Reuse work buffers across calls — they only grow on resize.
        if (static_cast<int>(ctx.frame_diff.size()) < y_size) ctx.frame_diff.resize(y_size);
        if (static_cast<int>(ctx.thresh.size())     < y_size) ctx.thresh.resize(y_size);

        CalculateFrameDiff(current_gray, ctx.previous_frame.data(),
                           ctx.frame_diff.data(), width, height);
        ThresholdBinary(ctx.frame_diff.data(), ctx.thresh.data(),
                        width, height, pixel_threshold);

        for (int roi_idx = 0; roi_idx < roi_count; roi_idx++) {
            const ROIRect& roi = roi_rects[roi_idx];

            const int roi_x1 = max(0, min(roi.x1, roi.x2));
            const int roi_x2 = min(width, max(roi.x1, roi.x2));
            const int roi_y1 = max(0, min(roi.y1, roi.y2));
            const int roi_y2 = min(height, max(roi.y1, roi.y2));

            int motion_pixels = 0;
            for (int y = roi_y1; y < roi_y2; y++) {
                for (int x = roi_x1; x < roi_x2; x++) {
                    if (ctx.thresh[y * width + x] > 0) {
                        motion_pixels++;
                    }
                }
            }

            if (motion_pixels >= min_area) {
                detected_roi_indices.push_back(roi_idx);

                cout << "[MotionDetector] Motion detected in ROI[" << roi_idx << "]: ("
                     << roi_x1 << ", " << roi_y1 << ") to (" << roi_x2 << ", " << roi_y2
                     << ") - " << motion_pixels << " changed pixels" << endl;
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
