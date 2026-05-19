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
    constexpr int kSubSample = 2;
}

MotionDetector::MotionDetector(int min_area, int threshold, int sensitivity)
    : m_minArea(min_area)
    , m_pixelThreshold(threshold)
    , m_sensitivity(sensitivity)
    , m_previousFrame(nullptr)
    , m_previousWidth(0)
    , m_previousHeight(0)
{
    cout << "[MotionDetector] Initialized with min_area=" << m_minArea 
         << ", pixel_threshold=" << m_pixelThreshold 
         << ", sensitivity=" << m_sensitivity << endl;
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
    // Convert confidence (0.0-1.0) to motion detection parameters
    // Lower confidence = more sensitive = detect more motion
    m_sensitivity = static_cast<int>((1.0f - threshold) * 100.0f);
    m_pixelThreshold = static_cast<int>(threshold * 50.0f);
    m_minArea = static_cast<int>(500 + threshold * 1500);
    
    cout << "[MotionDetector] Threshold set to " << threshold 
         << " -> sensitivity=" << m_sensitivity 
         << ", pixel_threshold=" << m_pixelThreshold 
         << ", min_area=" << m_minArea << endl;
}

void MotionDetector::SetThresholdAndSensitivity(int threshold, int sensitivity)
{
    // Clamp sensitivity to valid range
    m_sensitivity = max(0, min(100, sensitivity));
    
    // Convert threshold (0-100) to pixel difference threshold (0-50)
    // Higher threshold = higher pixel difference needed = less sensitive
    m_pixelThreshold = static_cast<int>((threshold / 100.0) * 50.0);
    
    // Convert sensitivity to min_area
    // Higher sensitivity = lower min_area = detect smaller motions
    // Scale: 100 sensitivity -> 300 px, 50 sensitivity -> 1000 px, 0 sensitivity -> 2000 px
    m_minArea = static_cast<int>(2000 - (m_sensitivity / 100.0) * 1700);
    
    cout << "[MotionDetector] Set threshold=" << threshold 
         << ", sensitivity=" << sensitivity 
         << " -> pixel_threshold=" << m_pixelThreshold 
         << ", min_area=" << m_minArea << endl;
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
        const int effective_min_area = max(1, m_minArea / subsample_factor);

        // Check each ROI for motion (fused diff + threshold + count, ROI-only)
        for (int roi_idx = 0; roi_idx < roi_count; roi_idx++) {
            const ROIRect& roi = roi_rects[roi_idx];

            // Ensure roi coordinates are in correct order and within bounds
            int roi_x1 = max(0, min(roi.x1, roi.x2));
            int roi_x2 = min(width, max(roi.x1, roi.x2));
            int roi_y1 = max(0, min(roi.y1, roi.y2));
            int roi_y2 = min(height, max(roi.y1, roi.y2));

            // Fused loop: compute |current - previous|, threshold, and count in one pass
            // over the ROI only. Early-exit once we've already exceeded the threshold.
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

                cout << "[MotionDetector] ✓ Motion detected in ROI[" << roi_idx << "]: ("
                     << roi_x1 << ", " << roi_y1 << ") to (" << roi_x2 << ", " << roi_y2
                     << ") - " << motion_pixels << " changed pixels (subsample=" << kSubSample << ")" << endl;
            }
        }

        // Update previous frame (whole Y plane: ROIs may change between calls)
        memcpy(m_previousFrame, current_gray, y_size);

        // // Cleanup of full-frame buffers (no longer allocated)
        // delete[] frame_diff;
        // delete[] thresh;
        
        if (detected_roi_indices.size() > 0) {
            cout << "[MotionDetector] ✓ Total: " << detected_roi_indices.size() << " ROI(s) with motion" << endl;
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
