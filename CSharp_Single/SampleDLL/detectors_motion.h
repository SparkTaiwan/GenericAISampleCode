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
     * Constructor
     * @param min_area Minimum contour area to be considered as motion
     * @param threshold Pixel difference threshold (0-255), higher = less sensitive to small changes
     * @param sensitivity Overall sensitivity (0-100), higher = more motion detection
     */
    MotionDetector(int min_area = 500, int threshold = 25, int sensitivity = 50);
    
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
     * Set threshold and sensitivity directly (0-100 scale)
     * @param threshold Detection threshold (0-100), higher = need bigger changes to detect motion
     * @param sensitivity Detection sensitivity (0-100), higher = more sensitive to motion
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
    int m_minArea;
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
