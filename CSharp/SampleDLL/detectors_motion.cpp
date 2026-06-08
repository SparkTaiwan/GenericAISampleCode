#include "pch.h"
#include "detectors_motion.h"
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cmath>

using namespace std;

namespace {
    // Subsample stride: process every kSubSample-th pixel in both x and y inside each ROI.
    // 2 -> 4x fewer pixels checked; visually indistinguishable for motion detection.
    // Set to 1 to disable subsampling (matches original per-pixel coverage).
    constexpr int kSubSample = 1;
}

MotionDetector::MotionDetector(int threshold, int sensitivity)
    : m_minRatio(0.0f)
    , m_pixelThreshold(0)
    , m_sensitivity(0)
    , m_previousFrame(nullptr)
    , m_previousWidth(0)
    , m_previousHeight(0)
{
    // Delegate so the constructor defaults and the SetThresholdAndSensitivity formula
    // can never disagree (the prior split caused a "first SetParameters call makes the
    // detector feel less sensitive" surprise even when the user passed sensitivity=50).
    SetThresholdAndSensitivity(threshold, sensitivity);
}

MotionDetector::~MotionDetector()
{
    if (m_previousFrame != nullptr) {
        delete[] m_previousFrame;
        m_previousFrame = nullptr;
    }
}

void MotionDetector::SetConfidenceThreshold(float threshold)
{
    // Lower confidence = more sensitive. Delegate so there's only one source of truth
    // for the motion parameters (the prior split kept two formulas in sync by hand).
    float c = max(0.0f, min(1.0f, threshold));
    int t = static_cast<int>(c * 100.0f);
    int s = static_cast<int>((1.0f - c) * 100.0f);
    SetThresholdAndSensitivity(t, s);
}

void MotionDetector::SetThresholdAndSensitivity(int threshold, int sensitivity)
{
    int t = max(0, min(100, threshold));
    m_sensitivity = max(0, min(100, sensitivity));

    // threshold 0..100 -> pixel diff 8..40. Keeping a non-zero floor avoids the prior
    // degenerate case where threshold=0 made every +/-1 compression artifact count as
    // motion; the upper bound matches the typical 25-40 range used in OpenCV tutorials.
    m_pixelThreshold = 8 + static_cast<int>((t / 100.0) * 32.0);

    // sensitivity 0..100 -> ROI area fraction 0.20..0.005 (20% .. 0.5%). Stored as a
    // ratio (not an absolute pixel count) so the same setting behaves consistently on
    // a 100x100 ROI and a 1920x1080 ROI; Detect() multiplies by each ROI's own area.
    // Wide range so the low end ignores small/distant motion (only large objects entering
    // the frame trigger) and the high end still catches subtle movement for surveillance.
    m_minRatio = 0.20f - (m_sensitivity / 100.0f) * 0.195f;

    cout << "[MotionDetector] Set threshold=" << threshold
         << ", sensitivity=" << sensitivity
         << " -> pixel_threshold=" << m_pixelThreshold
         << ", min_ratio=" << m_minRatio << endl;
}

void MotionDetector::Reset()
{
    if (m_previousFrame != nullptr) {
        delete[] m_previousFrame;
        m_previousFrame = nullptr;
    }
    m_previousWidth = 0;
    m_previousHeight = 0;
    cout << "[MotionDetector] State reset - previous frame cleared" << endl;
}

int MotionDetector::Detect(const unsigned char* yuv420_frame, int width, int height,
                          const ROIRect* roi_rects, int roi_count,
                          std::vector<int>& detected_roi_indices)
{
    detected_roi_indices.clear();
    
    try {
        // Motion detection requires ROI configuration
        if (roi_rects == nullptr || roi_count <= 0) {
            return 0;
        }
        
        // Calculate Y channel size
        int y_size = width * height;
        
        // Extract Y (luminance) channel as grayscale (first part of YUV420)
        const unsigned char* current_gray = yuv420_frame;
        
        // Need previous frame for comparison
        if (m_previousFrame == nullptr || m_previousWidth != width || m_previousHeight != height) {
            // Allocate and copy first frame
            if (m_previousFrame != nullptr) {
                delete[] m_previousFrame;
            }
            m_previousFrame = new unsigned char[y_size];
            memcpy(m_previousFrame, current_gray, y_size);
            m_previousWidth = width;
            m_previousHeight = height;
            return 0; // No motion on first frame
        }
        
        // --- Original full-frame two-pass approach (kept for reference) ---
        // // Calculate absolute difference between frames
        // unsigned char* frame_diff = new unsigned char[y_size];
        // CalculateFrameDiff(current_gray, m_previousFrame, frame_diff, width, height);
        //
        // // Apply threshold to get binary image
        // unsigned char* thresh = new unsigned char[y_size];
        // ThresholdBinary(frame_diff, thresh, width, height, m_pixelThreshold);
        // --- End original ---

        // Subsampling compensates min_area so the "fraction of ROI moving" stays equivalent.
        const int subsample_factor = kSubSample * kSubSample;

        // Check each ROI for motion (fused diff + threshold + count, ROI-only)
        for (int roi_idx = 0; roi_idx < roi_count; roi_idx++) {
            const ROIRect& roi = roi_rects[roi_idx];

            // Ensure roi coordinates are in correct order and within bounds
            int roi_x1 = max(0, min(roi.x1, roi.x2));
            int roi_x2 = min(width, max(roi.x1, roi.x2));
            int roi_y1 = max(0, min(roi.y1, roi.y2));
            int roi_y2 = min(height, max(roi.y1, roi.y2));

            // Per-ROI minimum area: m_minRatio (e.g. ~10% at sensitivity=50) of THIS ROI's
            // pixel count, so the same sensitivity setting fires consistently on small and
            // large ROIs (the prior absolute min_area made a 100x100 ROI need 10% motion and
            // a 1920x1080 ROI need 0.05% - same setting, wildly different behavior).
            const int roi_area = (roi_x2 - roi_x1) * (roi_y2 - roi_y1);
            const int effective_min_area = max(1, static_cast<int>(roi_area * m_minRatio) / subsample_factor);

            // Fused loop: compute |current - previous|, threshold, and count in one pass
            // over the ROI only. Early-exit once motion_pixels reaches effective_min_area
            // so a high-fps multi-channel deployment doesn't pay for a full 1080p scan on
            // every triggered frame. motion_pixels at the log point therefore reflects the
            // trigger threshold, not the full count - the log states this explicitly with
            // ">=" so a reader doesn't mistake it for the actual number of moving pixels.
            int motion_pixels = 0;
            bool roi_done = false;
            for (int y = roi_y1; y < roi_y2 && !roi_done; y += kSubSample) {
                const unsigned char* curr_row = current_gray + y * width;
                const unsigned char* prev_row = m_previousFrame + y * width;
                for (int x = roi_x1; x < roi_x2; x += kSubSample) {
                    int diff = static_cast<int>(curr_row[x]) - static_cast<int>(prev_row[x]);
                    if (abs(diff) > m_pixelThreshold) {
                        motion_pixels++;
                        if (motion_pixels >= effective_min_area) {
                            roi_done = true;
                            break;
                        }
                    }
                }
            }

            // Check if motion exceeds minimum threshold (compared on subsampled scale)
            if (motion_pixels >= effective_min_area) {
                // Return the ROI index
                detected_roi_indices.push_back(roi_idx);

                cout << "[MotionDetector] Motion detected in ROI[" << roi_idx << "]: ("
                     << roi_x1 << ", " << roi_y1 << ") to (" << roi_x2 << ", " << roi_y2
                     << ") - >=" << motion_pixels << " changed pixels (early-exit, full count not computed; "
                     << "subsample=" << kSubSample << ", roi_area=" << roi_area
                     << ", min_area=" << effective_min_area << ")" << endl;
            }
        }

        // Update previous frame
        memcpy(m_previousFrame, current_gray, y_size);

        // Cleanup
        // delete[] frame_diff;
        // delete[] thresh;
        
        if (detected_roi_indices.size() > 0) {
            cout << "[MotionDetector] Total: " << detected_roi_indices.size() << " ROI(s) with motion" << endl;
        }
        
        return static_cast<int>(detected_roi_indices.size());
        
    } catch (const exception& e) {
        cout << "[MotionDetector] Detection error: " << e.what() << endl;
        return 0;
    }
}

void MotionDetector::ThresholdBinary(const unsigned char* input, unsigned char* output,
                                    int width, int height, int threshold)
{
    int size = width * height;
    for (int i = 0; i < size; i++) {
        output[i] = (input[i] > threshold) ? 255 : 0;
    }
}

void MotionDetector::CalculateFrameDiff(const unsigned char* frame1, const unsigned char* frame2,
                                       unsigned char* output, int width, int height)
{
    int size = width * height;
    for (int i = 0; i < size; i++) {
        int diff = static_cast<int>(frame1[i]) - static_cast<int>(frame2[i]);
        output[i] = static_cast<unsigned char>(abs(diff));
    }
}
