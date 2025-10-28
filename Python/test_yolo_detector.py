#!/usr/bin/env python3
"""
Test script for YOLO detector initialization.
"""
import sys
import os

# Add the current directory to Python path
sys.path.insert(0, os.path.dirname(__file__))

from detectors import get_default_detector, YOLOHumanDetector

def test_detector():
    """Test the default detector initialization."""
    print("Testing detector initialization...")

    try:
        detector = get_default_detector()
        print(f"Detector type: {type(detector).__name__}")

        # Test with dummy data - create proper YUV420 format
        width, height = 640, 480
        y_size = width * height  # 307200
        uv_size = (width * height) // 4  # 76800 each for U and V
        total_size = y_size + 2 * uv_size  # 307200 + 153600 = 460800
        
        # Create YUV420 data (Y plane + UV interleaved)
        yuv420_data = bytearray(total_size)
        
        # Fill Y plane with gray values
        for i in range(y_size):
            yuv420_data[i] = 128  # Gray
        
        # Fill UV planes (interleaved UVUVUV...)
        for i in range(uv_size * 2):
            yuv420_data[y_size + i] = 128  # Neutral UV
        
        detections = detector.detect(bytes(yuv420_data), width, height)
        print(f"Dummy detection result: {len(detections)} detections")

        print("Detector test completed successfully!")
        return True

    except Exception as e:
        print(f"Detector test failed: {e}")
        import traceback
        traceback.print_exc()
        return False

def test_yolo_directly():
    """Test YOLO detector directly."""
    print("\nTesting YOLO detector directly...")

    try:
        detector = YOLOHumanDetector()
        print(f"YOLO detector initialized: {detector._model is not None}")

        if detector._model is not None:
            # Test with dummy data - create proper YUV420 format
            width, height = 640, 480
            y_size = width * height  # 307200
            uv_size = (width * height) // 4  # 76800 each for U and V
            total_size = y_size + 2 * uv_size  # 307200 + 153600 = 460800
            
            # Create YUV420 data (Y plane + UV interleaved)
            yuv420_data = bytearray(total_size)
            
            # Fill Y plane with gray values
            for i in range(y_size):
                yuv420_data[i] = 128  # Gray
            
            # Fill UV planes (interleaved UVUVUV...)
            for i in range(uv_size * 2):
                yuv420_data[y_size + i] = 128  # Neutral UV
            
            detections = detector.detect(bytes(yuv420_data), width, height)
            print(f"YOLO detection result: {len(detections)} detections")
        else:
            print("YOLO model not loaded, using fallback")

        print("YOLO test completed!")
        return True

    except Exception as e:
        print(f"YOLO test failed: {e}")
        import traceback
        traceback.print_exc()
        return False

def test_with_real_image():
    """Test with the real test.jpg image that contains a person"""
    print("\nTesting with real test.jpg image...")
    
    try:
        from PIL import Image
        import numpy as np
        import os
        
        image_path = "test.jpg"
        if not os.path.exists(image_path):
            print(f"Error: {image_path} not found")
            return False
            
        # Load the image
        rgb_image = Image.open(image_path)
        rgb_array = np.array(rgb_image)
        
        print(f"Loaded image: {rgb_array.shape}")
        
        # Convert to YUV420 format
        yuv420_data = rgb_to_yuv420(rgb_array)
        
        # Test with YOLO detector
        detector = get_default_detector()
        height, width = rgb_array.shape[:2]
        
        print(f"Testing YOLO detection on {width}x{height} image...")
        detections = detector.detect(yuv420_data, width, height)
        
        print(f"Real image test - Detections: {len(detections)}")
        
        if detections:
            print("Detected persons:")
            for i, (x, y, w, h) in enumerate(detections):
                print(f"  Person {i+1}: position=({x},{y}) size=({w},{h})")
        
        return len(detections) > 0
        
    except Exception as e:
        print(f"Error testing real image: {e}")
        import traceback
        traceback.print_exc()
        return False

def rgb_to_yuv420(rgb_image):
    """Convert RGB image to YUV420 format"""
    import numpy as np
    
    height, width = rgb_image.shape[:2]
    
    # Convert RGB to YUV
    r, g, b = rgb_image[:, :, 0].astype(np.float32), rgb_image[:, :, 1].astype(np.float32), rgb_image[:, :, 2].astype(np.float32)
    
    y = 0.299 * r + 0.587 * g + 0.114 * b
    u = -0.147 * r - 0.289 * g + 0.436 * b + 128
    v = 0.615 * r - 0.515 * g - 0.100 * b + 128
    
    # Clip to valid range
    y = np.clip(y, 0, 255).astype(np.uint8)
    u = np.clip(u, 0, 255).astype(np.uint8)
    v = np.clip(v, 0, 255).astype(np.uint8)
    
    # Subsample U and V
    u_sub = u[::2, ::2]
    v_sub = v[::2, ::2]
    
    # Interleave U and V (NV12 format)
    uv_interleaved = np.empty((u_sub.shape[0], u_sub.shape[1] * 2), dtype=np.uint8)
    uv_interleaved[:, 0::2] = u_sub
    uv_interleaved[:, 1::2] = v_sub
    
    # Combine Y, U, V
    y_flat = y.flatten()
    uv_flat = uv_interleaved.flatten()
    
    return bytes(y_flat) + bytes(uv_flat)

if __name__ == "__main__":
    success1 = test_detector()
    success2 = test_yolo_directly()
    success3 = test_with_real_image()
    sys.exit(0 if (success1 and success2 and success3) else 1)