"""
Image processing module - corresponds to image conversion functionality in C#
"""
import numpy as np
from PIL import Image, ImageDraw
import base64
import io
import time
from typing import Tuple, List

class ImageProcessor:
    """Image processing class"""
    
    @staticmethod
    def yuv420_to_rgb(yuv_data: bytes, width: int, height: int) -> np.ndarray:
        """
        Convert YUV420 format to RGB
        Corresponds to ConvertYUV420ToBitmap method in C#
        """
        yuv_array = np.frombuffer(yuv_data, dtype=np.uint8)
        
        frame_size = width * height
        chroma_size = frame_size // 4
        
        if len(yuv_array) != frame_size + 2 * chroma_size:
            raise ValueError(f"Invalid YUV420 data size. Expected {frame_size + 2 * chroma_size}, got {len(yuv_array)}")
        
        # Separate Y, U, V components
        y = yuv_array[:frame_size].reshape((height, width))
        u = yuv_array[frame_size:frame_size + chroma_size].reshape((height//2, width//2))
        v = yuv_array[frame_size + chroma_size:].reshape((height//2, width//2))
        
        # Upsample U and V components
        u_upsampled = np.repeat(np.repeat(u, 2, axis=0), 2, axis=1)
        v_upsampled = np.repeat(np.repeat(v, 2, axis=0), 2, axis=1)
        
        # Convert to RGB
        rgb_array = np.zeros((height, width, 3), dtype=np.uint8)
        
        # YUV to RGB conversion formula
        C = y.astype(np.float32) - 16
        D = u_upsampled.astype(np.float32) - 128
        E = v_upsampled.astype(np.float32) - 128
        
        R = (298 * C + 409 * E + 128) / 256
        G = (298 * C - 100 * D - 208 * E + 128) / 256
        B = (298 * C + 516 * D + 128) / 256
        
        # Clip to 0-255 range
        rgb_array[:, :, 0] = np.clip(R, 0, 255)
        rgb_array[:, :, 1] = np.clip(G, 0, 255)
        rgb_array[:, :, 2] = np.clip(B, 0, 255)
        
        return rgb_array
    
    @staticmethod
    def yuv420_to_base64_jpeg(yuv_data: bytes, width: int, height: int, quality: int = 50, 
                             detections: List[Tuple[int, int, int, int]] = None, debug_mode: bool = False) -> str:
        """
        Convert YUV420 directly to Base64 encoded JPEG
        Corresponds to ConvertYUV420ToBase64Jpeg method in C#
        If detections are provided, draw red boxes around detected objects
        If debug_mode is True, save detection images to disk
        """
        # Convert to RGB
        rgb_array = ImageProcessor.yuv420_to_rgb(yuv_data, width, height)
        
        # Create PIL image
        image = Image.fromarray(rgb_array, 'RGB')
        
        # Draw red boxes around detected objects if any
        if detections:
            draw = ImageDraw.Draw(image)
            for detection in detections:
                if len(detection) == 4:  # (x, y, w, h) format
                    x, y, w, h = detection
                    draw.rectangle([x, y, x + w, y + h], outline=(255, 0, 0), width=2)
            
            # Save the image with red boxes only in debug mode
            if debug_mode:
                import datetime
                now = datetime.datetime.now()
                timestamp_str = now.strftime("%Y%m%d_%H%M%S_%f")[:-3]  # Include milliseconds
                detection_filename = f"debug_detection_{timestamp_str}.jpg"
                image.save(detection_filename, format='JPEG', quality=quality, optimize=True)
                print(f"[ImageProcessor] Saved detection image: {detection_filename} with {len(detections)} boxes")
        
        # Convert to JPEG and encode as Base64
        buffer = io.BytesIO()
        image.save(buffer, format='JPEG', quality=quality, optimize=True)
        jpeg_bytes = buffer.getvalue()
        
        return base64.b64encode(jpeg_bytes).decode('utf-8')
    
    @staticmethod
    def create_test_yuv420_image(width: int, height: int) -> bytes:
        """
        Create test YUV420 image data
        """
        frame_size = width * height
        chroma_size = frame_size // 4
        
        # Create simple test pattern
        y_data = np.random.randint(0, 256, frame_size, dtype=np.uint8)
        u_data = np.full(chroma_size, 128, dtype=np.uint8)  # Neutral chroma
        v_data = np.full(chroma_size, 128, dtype=np.uint8)
        
        return y_data.tobytes() + u_data.tobytes() + v_data.tobytes()

    @staticmethod
    def create_test_base64_jpeg(width: int, height: int, text: str = "Test") -> str:
        """
        Create test Base64 JPEG image
        Used for debug mode fake data reporting
        """
        from PIL import Image, ImageDraw, ImageFont
        import io
        import base64
        
        # Create test image
        image = Image.new('RGB', (width, height), color=(100, 150, 200))
        draw = ImageDraw.Draw(image)
        
        # Try to add text
        try:
            # Draw a rectangle and text in the center of the image
            rect_x = width // 4
            rect_y = height // 4
            rect_w = width // 2
            rect_h = height // 2
            
            draw.rectangle([rect_x, rect_y, rect_x + rect_w, rect_y + rect_h], 
                          outline=(255, 255, 0), width=3)
            
            # Add text
            text_x = width // 2 - len(text) * 5
            text_y = height // 2
            draw.text((text_x, text_y), text, fill=(255, 255, 255))
            
        except Exception:
            pass  # If font or drawing fails, use solid color background
        
        # Convert to JPEG and encode as Base64
        buffer = io.BytesIO()
        image.save(buffer, format='JPEG', quality=50, optimize=True)
        jpeg_bytes = buffer.getvalue()
        
        return base64.b64encode(jpeg_bytes).decode('utf-8')
