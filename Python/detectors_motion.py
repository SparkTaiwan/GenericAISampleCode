"""
Motion detection module with threshold and sensitivity support.
Detects motion by comparing consecutive frames.
"""
from typing import List, Tuple
import numpy as np

class BaseDetector:
    """Abstract detector interface.

    detect returns a list of bounding boxes (x, y, w, h) in pixel coordinates.
    """
    def detect(self, yuv420_frame: bytes, width: int, height: int, 
               roi_rects: List[Tuple[int, int, int, int]] = None) -> List[Tuple[int, int, int, int]]:
        raise NotImplementedError
    
    def set_confidence_threshold(self, threshold: float):
        """Set the confidence threshold for detection"""
        pass


class MotionDetector(BaseDetector):
    """Motion detector using frame differencing.
    
    Detects motion by comparing consecutive frames and identifying changed regions.
    Supports threshold and sensitivity parameters for tuning detection.
    """
    def __init__(self, min_area=500, threshold=25, sensitivity=50):
        """
        Args:
            min_area: Minimum contour area to be considered as motion
            threshold: Pixel difference threshold (0-255), higher = less sensitive to small changes
            sensitivity: Overall sensitivity (0-100), higher = more motion detection
        """
        self.min_area = min_area
        self.pixel_threshold = threshold
        self.sensitivity = sensitivity
        self.previous_frame = None
        self.previous_width = None
        self.previous_height = None
        
        print(f"[MotionDetector] Initialized with min_area={min_area}, pixel_threshold={threshold}, sensitivity={sensitivity}")
    
    def set_confidence_threshold(self, threshold: float):
        """Set the confidence threshold for detection (0.0-1.0)
        Maps to pixel_threshold and sensitivity for motion detection
        """
        # Convert confidence (0.0-1.0) to motion detection parameters
        # Lower confidence = more sensitive = detect more motion
        self.sensitivity = int((1.0 - threshold) * 100)  # Higher sensitivity for lower confidence
        self.pixel_threshold = int(threshold * 50)  # Lower pixel threshold for lower confidence
        self.min_area = int(500 + threshold * 1500)  # Smaller min area for lower confidence
        
        print(f"[MotionDetector] Threshold set to {threshold:.3f} -> sensitivity={self.sensitivity}, pixel_threshold={self.pixel_threshold}, min_area={self.min_area}")
    
    def set_threshold_and_sensitivity(self, threshold: int, sensitivity: int):
        """Set threshold and sensitivity directly (0-100 scale)
        
        Args:
            threshold: Detection threshold (0-100), higher = need bigger changes to detect motion
            sensitivity: Detection sensitivity (0-100), higher = more sensitive to motion
        """
        self.sensitivity = max(0, min(100, sensitivity))
        
        # Convert threshold (0-100) to pixel difference threshold (0-50)
        # Higher threshold = higher pixel difference needed = less sensitive
        self.pixel_threshold = int((threshold / 100.0) * 50)
        
        # Convert sensitivity to min_area
        # Higher sensitivity = lower min_area = detect smaller motions
        # Scale: 100 sensitivity -> 300 px, 50 sensitivity -> 1000 px, 0 sensitivity -> 2000 px
        self.min_area = int(2000 - (self.sensitivity / 100.0) * 1700)
        
        print(f"[MotionDetector] Set threshold={threshold}, sensitivity={sensitivity} -> pixel_threshold={self.pixel_threshold}, min_area={self.min_area}")
    
    def reset(self):
        """Reset detector state (clear previous frame)"""
        self.previous_frame = None
        self.previous_width = None
        self.previous_height = None
        print("[MotionDetector] State reset - previous frame cleared")
    
    def detect(self, yuv420_frame: bytes, width: int, height: int, 
               roi_rects: List[Tuple[int, int, int, int]] = None) -> List[Tuple[int, int, int, int]]:
        """Detect motion in ROI regions only.
        
        Returns list of ROI boxes (x, y, w, h) that have motion detected inside them.
        If no ROI is set, returns empty list (motion detection requires ROI configuration).
        """
        try:
            # Motion detection requires ROI configuration
            if roi_rects is None or len(roi_rects) == 0:
                return []
            
            # Convert YUV420 to grayscale (just use Y channel)
            y_size = width * height
            if len(yuv420_frame) < y_size:
                print(f"[MotionDetector] Not enough data: got {len(yuv420_frame)} bytes, need at least {y_size}")
                return []
            
            # Extract Y (luminance) channel as grayscale
            current_gray = np.frombuffer(yuv420_frame, dtype=np.uint8, count=y_size).reshape((height, width))
            
            # Need previous frame for comparison
            if self.previous_frame is None or self.previous_width != width or self.previous_height != height:
                self.previous_frame = current_gray.copy()
                self.previous_width = width
                self.previous_height = height
                return []  # No motion on first frame
            
            # Calculate absolute difference between frames
            frame_diff = np.abs(current_gray.astype(np.int16) - self.previous_frame.astype(np.int16)).astype(np.uint8)
            
            # Apply threshold to get binary image
            _, thresh = self._threshold_binary(frame_diff, self.pixel_threshold)
            
            # Check each ROI for motion
            detections = []
            for roi in roi_rects:
                if len(roi) >= 4:
                    roi_x1, roi_y1, roi_x2, roi_y2 = roi[:4]
                    
                    # Ensure roi coordinates are in correct order and within bounds
                    roi_x1, roi_x2 = max(0, min(roi_x1, roi_x2)), min(width, max(roi_x1, roi_x2))
                    roi_y1, roi_y2 = max(0, min(roi_y1, roi_y2)), min(height, max(roi_y1, roi_y2))
                    
                    # Extract ROI region from threshold image
                    roi_thresh = thresh[roi_y1:roi_y2, roi_x1:roi_x2]
                    
                    # Count motion pixels in ROI
                    motion_pixels = np.sum(roi_thresh > 0)
                    
                    # Check if motion exceeds minimum threshold
                    if motion_pixels >= self.min_area:
                        # Return the ROI itself (x, y, w, h format)
                        roi_w = roi_x2 - roi_x1
                        roi_h = roi_y2 - roi_y1
                        detections.append((roi_x1, roi_y1, roi_w, roi_h))
                        print(f"[MotionDetector] ✓ Motion detected in ROI: ({roi_x1}, {roi_y1}, {roi_w}, {roi_h}) - {motion_pixels} changed pixels")
            
            # Update previous frame
            self.previous_frame = current_gray.copy()
            
            if len(detections) > 0:
                print(f"[MotionDetector] ✓ Total: {len(detections)} ROI(s) with motion")
            
            return detections
            
        except Exception as e:
            print(f"[MotionDetector] Detection error: {e}")
            import traceback
            traceback.print_exc()
            return []
    
    def _threshold_binary(self, img: np.ndarray, threshold: int) -> Tuple[float, np.ndarray]:
        """Apply binary threshold to image"""
        binary = (img > threshold).astype(np.uint8) * 255
        return float(threshold), binary
    
    def _morphology_close(self, img: np.ndarray, kernel: np.ndarray) -> np.ndarray:
        """Morphological closing (dilation followed by erosion)"""
        dilated = self._dilate(img, kernel)
        closed = self._erode(dilated, kernel)
        return closed
    
    def _morphology_open(self, img: np.ndarray, kernel: np.ndarray) -> np.ndarray:
        """Morphological opening (erosion followed by dilation)"""
        eroded = self._erode(img, kernel)
        opened = self._dilate(eroded, kernel)
        return opened
    
    def _dilate(self, img: np.ndarray, kernel: np.ndarray) -> np.ndarray:
        """Simple dilation operation"""
        from scipy.ndimage import maximum_filter
        kh, kw = kernel.shape
        return maximum_filter(img, size=(kh, kw))
    
    def _erode(self, img: np.ndarray, kernel: np.ndarray) -> np.ndarray:
        """Simple erosion operation"""
        from scipy.ndimage import minimum_filter
        kh, kw = kernel.shape
        return minimum_filter(img, size=(kh, kw))
    
    def _find_contours(self, binary_img: np.ndarray) -> List[np.ndarray]:
        """Find contours in binary image using simple connected components"""
        from scipy.ndimage import label
        
        # Label connected components
        labeled, num_features = label(binary_img > 0)
        
        contours = []
        for i in range(1, num_features + 1):
            # Get pixels belonging to this component
            points = np.argwhere(labeled == i)
            if len(points) > 0:
                # Store as contour (swap y,x to x,y)
                contour = points[:, [1, 0]]  # Convert (row, col) to (x, y)
                contours.append(contour)
        
        return contours
    
    def _contour_area(self, contour: np.ndarray) -> float:
        """Calculate area of contour (number of pixels)"""
        return len(contour)
    
    def _bounding_rect(self, contour: np.ndarray) -> Tuple[int, int, int, int]:
        """Get bounding rectangle of contour"""
        x_min = int(np.min(contour[:, 0]))
        x_max = int(np.max(contour[:, 0]))
        y_min = int(np.min(contour[:, 1]))
        y_max = int(np.max(contour[:, 1]))
        
        w = x_max - x_min + 1
        h = y_max - y_min + 1
        
        return x_min, y_min, w, h
    
    def _is_detection_in_roi(self, det_x: int, det_y: int, det_w: int, det_h: int, 
                           roi_rects: List[Tuple[int, int, int, int]]) -> bool:
        """
        Check if a detection overlaps with any ROI rectangle
        Detection: (x, y, w, h) - top-left corner and size
        ROI: List of (x1, y1, x2, y2) rectangles where (x1,y1) and (x2,y2) are two corner points
        """
        det_x1, det_y1 = det_x, det_y
        det_x2, det_y2 = det_x + det_w, det_y + det_h
        
        for roi in roi_rects:
            if len(roi) >= 4:
                roi_x1, roi_y1, roi_x2, roi_y2 = roi[:4]
                
                # Ensure roi coordinates are in correct order (top-left, bottom-right)
                roi_x1, roi_x2 = min(roi_x1, roi_x2), max(roi_x1, roi_x2)
                roi_y1, roi_y2 = min(roi_y1, roi_y2), max(roi_y1, roi_y2)
                
                # Check if detection overlaps with ROI (intersection test)
                if (det_x1 < roi_x2 and det_x2 > roi_x1 and 
                    det_y1 < roi_y2 and det_y2 > roi_y1):
                    return True
        
        return False


def convert_threshold_to_confidence(threshold: int, sensitivity: int) -> float:
    """
    Convert threshold and sensitivity parameters to confidence value for compatibility
    
    Args:
        threshold: Detection threshold (0-100), higher means stricter
        sensitivity: Detection sensitivity (0-100), higher means more sensitive
    
    Returns:
        confidence: Confidence threshold (0.0-1.0)
    """
    # Ensure values are in valid range
    threshold = max(0, min(100, threshold))
    sensitivity = max(0, min(100, sensitivity))
    
    # Higher sensitivity = lower confidence required = more detections
    # Higher threshold = higher confidence required = fewer detections
    
    # Convert sensitivity to a multiplier (higher sensitivity = lower confidence)
    sensitivity_factor = (100 - sensitivity) / 100.0  # 0.0 to 1.0
    
    # Convert threshold to base confidence 
    threshold_factor = threshold / 100.0  # 0.0 to 1.0
    
    # Combine with reasonable weights
    base_confidence = (sensitivity_factor * 0.3 + threshold_factor * 0.7)
    
    # Map to practical range (0.1 to 0.8)
    confidence = 0.1 + (base_confidence * 0.7)
    
    return round(confidence, 3)


def get_default_detector() -> BaseDetector:
    """Get the default motion detector.
    
    Returns MotionDetector configured with default parameters.
    """
    try:
        detector = MotionDetector()
        print("[get_default_detector] Using MotionDetector")
        return detector
    except Exception as e:
        print(f"[get_default_detector] Failed to create MotionDetector: {e}")
        # Fallback to basic implementation
        class MockDetector(BaseDetector):
            def detect(self, yuv420_frame: bytes, width: int, height: int, 
                      roi_rects: List[Tuple[int, int, int, int]] = None) -> List[Tuple[int, int, int, int]]:
                return []
        return MockDetector()
