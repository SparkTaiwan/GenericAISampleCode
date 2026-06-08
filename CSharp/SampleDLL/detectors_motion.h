#pragma once
#include <vector>
#include <cstdint>

// ROI structure (x, y, w, h)
struct DetectionRect {
    int x;
    int y;
    int w;
    int h;
};

// ROI input structure (x1, y1, x2, y2) - two corner points
struct ROIRect {
    int x1;
    int y1;
    int x2;
    int y2;
};

/**
 * Motion detector using frame differencing.
 * 
 * Detects motion by comparing consecutive frames and identifying changed regions.
 * Supports threshold and sensitivity parameters for tuning detection.
 */
class MotionDetector {
public:
    /**
     * Constructor. Delegates to SetThresholdAndSensitivity so defaults always match the formula.
     * @param threshold Detection threshold (0-100), higher = bigger pixel diff needed (less sensitive)
     * @param sensitivity Detection sensitivity (0-100), higher = smaller ROI fraction triggers (more sensitive)
     */
    MotionDetector(int threshold = 50, int sensitivity = 50);
    
    /**
     * Destructor
     */
    ~MotionDetector();
    
    /**
     * Set the confidence threshold for detection (0.0-1.0)
     * Maps to pixel_threshold and sensitivity for motion detection
     * @param threshold Confidence threshold (0.0-1.0)
     */
    void SetConfidenceThreshold(float threshold);
    
    /**
     * Set threshold and sensitivity (0-100 scale).
     * threshold maps to m_pixelThreshold (8..40 gray-level diff); sensitivity maps to
     * m_minRatio (0.05..0.005 of ROI area). Higher threshold = less sensitive to small
     * brightness changes; higher sensitivity = smaller fraction of ROI needed to trigger.
     * @param threshold Detection threshold (0-100), higher = need bigger pixel diff to detect motion
     * @param sensitivity Detection sensitivity (0-100), higher = smaller fraction of ROI triggers motion
     */
    void SetThresholdAndSensitivity(int threshold, int sensitivity);
    
    /**
     * Reset detector state (clear previous frame)
     */
    void Reset();
    
    /**
     * Detect motion in ROI regions only.
     * 
     * @param yuv420_frame YUV420 frame data
     * @param width Frame width
     * @param height Frame height
     * @param roi_rects List of ROI rectangles (x1, y1, x2, y2)
     * @param roi_count Number of ROI rectangles
     * @param detected_roi_indices Output vector of ROI indices that have motion
     * @return Number of detections found
     */
    int Detect(const unsigned char* yuv420_frame, int width, int height,
               const ROIRect* roi_rects, int roi_count,
               std::vector<int>& detected_roi_indices);

private:
    // Minimum fraction of each ROI's area (0.0-1.0) that must change to count as motion.
    // Normalized per-ROI so the same sensitivity setting behaves the same on small and large ROIs.
    float m_minRatio;
    int m_pixelThreshold;
    int m_sensitivity;
    
    unsigned char* m_previousFrame;
    int m_previousWidth;
    int m_previousHeight;
    
    /**
     * Apply binary threshold to image
     * @param input Input grayscale image
     * @param output Output binary image
     * @param width Image width
     * @param height Image height
     * @param threshold Threshold value
     */
    void ThresholdBinary(const unsigned char* input, unsigned char* output, 
                        int width, int height, int threshold);
    
    /**
     * Calculate absolute difference between two frames
     * @param frame1 First frame
     * @param frame2 Second frame
     * @param output Output difference image
     * @param width Image width
     * @param height Image height
     */
    void CalculateFrameDiff(const unsigned char* frame1, const unsigned char* frame2,
                           unsigned char* output, int width, int height);
};
