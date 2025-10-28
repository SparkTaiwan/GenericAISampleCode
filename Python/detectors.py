"""
Detector module with a pluggable interface and a default human detector.
"""
from typing import List, Tuple

# Lazy import guards for optional dependencies
_yolo = None

def _ensure_yolo():
    global _yolo
    if _yolo is None:
        try:
            from ultralytics import YOLO  # type: ignore
            _yolo = YOLO
        except Exception:
            _yolo = None
    return _yolo

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

class MockDetector(BaseDetector):
    """No-op detector used as fallback when dependencies are missing."""
    def detect(self, yuv420_frame: bytes, width: int, height: int, 
               roi_rects: List[Tuple[int, int, int, int]] = None) -> List[Tuple[int, int, int, int]]:
        print("[MockDetector] No detection performed.")
        return []


class YOLOHumanDetector(BaseDetector):
    """Modern YOLO-based human detector using Ultralytics YOLOv8.
    
    This is much faster and more accurate than traditional methods.
    Requires: pip install ultralytics
    """
    def __init__(self, model_size='n', confidence_threshold=0.3):  # n=tiny, s=small, m=medium, l=large, x=xlarge
        YOLO = _ensure_yolo()
        if YOLO is None:
            self._delegate = MockDetector()
            self._model = None
            print("[YOLOHumanDetector] Ultralytics YOLO not available. Install with: pip install ultralytics")
            return
        
        self.confidence_threshold = confidence_threshold
        
        try:
            # Load YOLOv8 model (will download automatically if not present)
            model_name = f'yolov8{model_size}.pt'
            print(f"[YOLOHumanDetector] Loading YOLOv8-{model_size} model...")
            
            # Try loading with default settings first
            try:
                self._model = YOLO(model_name)
            except Exception as load_error:
                if "weights_only" in str(load_error) or "WeightsUnpickler" in str(load_error):
                    print(f"[YOLOHumanDetector] Secure loading failed, using trusted fallback...")
                    # For trusted ultralytics models, allow unsafe loading
                    import torch
                    # Temporarily patch torch.load to disable weights_only
                    original_load = torch.load
                    def patched_load(*args, **kwargs):
                        kwargs['weights_only'] = False
                        return original_load(*args, **kwargs)
                    torch.load = patched_load
                    try:
                        self._model = YOLO(model_name)
                    finally:
                        torch.load = original_load
                else:
                    raise load_error
            
            print(f"[YOLOHumanDetector] YOLOv8-{model_size} model loaded successfully")
        except Exception as e:
            print(f"[YOLOHumanDetector] Failed to load YOLO model: {e}")
            self._delegate = MockDetector()
            self._model = None

    def set_confidence_threshold(self, threshold: float):
        """Set the confidence threshold for detection"""
        self.confidence_threshold = threshold
        print(f"[YOLOHumanDetector] Confidence threshold set to {threshold}")

    def detect(self, yuv420_frame: bytes, width: int, height: int, 
               roi_rects: List[Tuple[int, int, int, int]] = None) -> List[Tuple[int, int, int, int]]:
        # Delegate if YOLO isn't available
        if self._model is None:
            return self._delegate.detect(yuv420_frame, width, height)

        import numpy as np

        y_size = width * height
        uv_size = (width * height) // 4  # U and V planes are quarter size
        total_size = y_size + 2 * uv_size

        if len(yuv420_frame) < total_size:
            print(f"[YOLOHumanDetector] Not enough data: got {len(yuv420_frame)} bytes, need at least {total_size}")
            return []

        try:
            # Parse YUV420 data
            y = np.frombuffer(yuv420_frame, dtype=np.uint8, count=y_size).reshape((height, width))
            
            # U and V planes are interleaved and subsampled
            uv_start = y_size
            uv_data = np.frombuffer(yuv420_frame, dtype=np.uint8, offset=uv_start, count=2*uv_size)
            
            # Reshape UV data (NV12 format: UVUVUV...)
            uv_plane = uv_data.reshape((height//2, width//2, 2))
            u = uv_plane[:, :, 0]  # U plane
            v = uv_plane[:, :, 1]  # V plane
            
            # Upsample U and V to full resolution
            u_upsampled = np.kron(u, np.ones((2, 2), dtype=np.uint8))
            v_upsampled = np.kron(v, np.ones((2, 2), dtype=np.uint8))
            
            # Convert YUV to RGB
            # YUV to RGB conversion formulas
            y_float = y.astype(np.float32)
            u_float = u_upsampled.astype(np.float32) - 128
            v_float = v_upsampled.astype(np.float32) - 128
            
            r = y_float + 1.402 * v_float
            g = y_float - 0.344136 * u_float - 0.714136 * v_float
            b = y_float + 1.772 * u_float
            
            # Clip to valid range and convert to uint8
            rgb = np.stack([r, g, b], axis=-1)
            rgb = np.clip(rgb, 0, 255).astype(np.uint8)
            
            # Debug: Save converted image for inspection only when persons are detected
            #import datetime
            #now = datetime.datetime.now()
            #debug_filename = f"debug_yuv2rgb_{now.strftime('%Y%m%d_%H%M%S_%f')[:-3]}.jpg"
            #from PIL import Image
            
            # Run YOLO inference with configurable confidence threshold
            results = self._model(rgb, conf=self.confidence_threshold, verbose=False)
            
            #print(f"[YOLOHumanDetector] Running detection with confidence threshold: {self.confidence_threshold}")
            
            detections = []
            if results and len(results) > 0:
                result = results[0]
                
                total_objects = len(result.boxes) if result.boxes is not None else 0
                #print(f"[YOLOHumanDetector] YOLO found {total_objects} total objects")
                
                # Filter for person class (class 0 in COCO dataset)
                if result.boxes is not None:
                    person_count = 0
                    for i, box in enumerate(result.boxes):
                        class_id = int(box.cls.item())
                        confidence = box.conf.item()
                        
                        # Check if it's a person (class 0)
                        if class_id == 0:  # person class
                            person_count += 1
                            #print(f"[YOLOHumanDetector] Person {person_count}: conf={confidence:.3f}, threshold={self.confidence_threshold}")
                            
                            if confidence > self.confidence_threshold:
                                x1, y1, x2, y2 = box.xyxy[0].cpu().numpy()
                                
                                # Convert to (x, y, w, h) format
                                x, y = int(x1), int(y1)
                                w, h = int(x2 - x1), int(y2 - y1)
                                
                                # Check if detection is within any ROI (if ROI filtering is enabled)
                                if roi_rects is None or self._is_detection_in_roi(x, y, w, h, roi_rects):
                                    detections.append((x, y, w, h))
                                    #print(f"[YOLOHumanDetector] ✓ Person accepted: ({x},{y},{w},{h}) conf={confidence:.3f}")
                                #else:
                                #    print(f"[YOLOHumanDetector] ✗ Person outside ROI: ({x},{y},{w},{h}) conf={confidence:.3f}")
                            #else:
                            #    print(f"[YOLOHumanDetector] ✗ Person below threshold: conf={confidence:.3f} < {self.confidence_threshold}")
                    
                    #if person_count == 0:
                    #    print(f"[YOLOHumanDetector] No persons detected in {total_objects} objects")
            #else:
            #    print(f"[YOLOHumanDetector] No objects detected by YOLO")

            if len(detections) > 0:
                print(f"[YOLOHumanDetector] ✓ Final result: {len(detections)} persons detected and accepted")
            #else:
            #    print(f"[YOLOHumanDetector] ✗ Final result: No persons met all criteria")
            return detections
            
        except Exception as e:
            print(f"[YOLOHumanDetector] Detection error: {e}")
            import traceback
            traceback.print_exc()
            return []

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
    Convert threshold and sensitivity parameters to YOLO confidence value
    
    Args:
        threshold: Detection threshold (0-100), higher means stricter
        sensitivity: Detection sensitivity (0-100), higher means more sensitive
    
    Returns:
        confidence: YOLO confidence threshold (0.0-1.0)
    """
    # Ensure values are in valid range
    threshold = max(0, min(100, threshold))
    sensitivity = max(0, min(100, sensitivity))
    
    # Revised calculation for more reasonable confidence values
    # Higher sensitivity = lower confidence required = more detections
    # Higher threshold = higher confidence required = fewer detections
    
    # Convert sensitivity to a multiplier (higher sensitivity = lower confidence)
    sensitivity_factor = (100 - sensitivity) / 100.0  # 0.0 to 1.0
    
    # Convert threshold to base confidence 
    threshold_factor = threshold / 100.0  # 0.0 to 1.0
    
    # Combine with reasonable weights
    # More weight on sensitivity (detection rate) than threshold (precision)
    base_confidence = (sensitivity_factor * 0.3 + threshold_factor * 0.7)
    
    # Map to practical YOLO confidence range (0.05 to 0.6)
    # 0.05 = very permissive (catches almost everything)
    # 0.6 = quite strict (only high-confidence detections)
    confidence = 0.05 + (base_confidence * 0.55)
    
    return round(confidence, 3)


def get_default_detector() -> BaseDetector:
    """Get the default human detector.
    
    Uses YOLO for best accuracy and performance.
    Falls back to Mock detector if YOLO is unavailable.
    """
    try:
        # Try YOLO first (modern, fast, accurate)
        detector = YOLOHumanDetector()
        if detector._model is not None:
            return detector
        print("[get_default_detector] YOLO not available, falling back to Mock detector")
    except Exception as e:
        print(f"[get_default_detector] YOLO failed: {e}, falling back to Mock detector")
    
    # Fallback to Mock detector
    return MockDetector()

