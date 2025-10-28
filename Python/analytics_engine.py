import ctypes
import threading
import mmap
import struct
import time
from ctypes import Structure, c_int, c_char, c_char_p, c_uint64, c_ubyte
from detectors import get_default_detector, BaseDetector
from data_structures import SettingParameters, ROIGroup


# ---------- Struct definitions ----------
class ROI(Structure):
    _fields_ = [("x", c_int),
                ("y", c_int)]

# Legacy structure for compatibility with shared memory
class LegacySettingParameters(Structure):
    _fields_ = [
        ("version", c_char * 32),
        ("analytics_event_api_url", c_char * 256),
        ("image_width", c_int),
        ("image_height", c_int),
        ("jpg_compress", c_int),
        ("sensitivity", c_int * 10),
        ("threshold", c_int * 10),
        ("rois", ROI * 100)  # 10x10 flatten
    ]


import mmap
import struct

MMF_DATA_HEADER = 0x1234
MMF_DATA_FOOTER = 0x4321
MMF_DATA_SIZE = (8 + 4 + 4 + 4 + 4 + 8 + (1920 * 1080 * 3) + 8)

# ---------- Global variables ----------
g_portnum = 0
g_running = True
g_bgThread = None
g_callbackFunction = None
g_isSetting = False
g_url = ""
g_mtx = threading.Lock()
g_detector: BaseDetector = None  # type: ignore
g_roi_rects = []  # Store ROI rectangles for detection filtering

# ---------- MMF reading ----------

class MMF_Data:
    def __init__(self, raw_bytes: bytes):
        (
            self.header,
            self.image_status,
            self.image_width,
            self.image_height,
            self.image_size,
            self.timestamp,
        ) = struct.unpack_from("<QiiiIQ", raw_bytes, 0)

        # Image data
        self.image_data = raw_bytes[32:32 + self.image_size]

        # Footer after image buffer
        footer_offset = 32 + (1920 * 1080 * 3)
        self.footer, = struct.unpack_from("<q", raw_bytes, footer_offset)


g_hMap = None  # Python doesn't need HANDLE, mmap object is sufficient

def get_mmf(frame_holder, width_holder, height_holder, size_holder, timestamp_holder):
    global g_hMap

    mmf_name = f"ChannelFrame_{g_portnum}"
    try:
        if g_hMap is None:
            # ACCESS_WRITE to be able to change image_status
            g_hMap = mmap.mmap(-1, MMF_DATA_SIZE, tagname=mmf_name, access=mmap.ACCESS_WRITE)
            print(f"Opened shared mem: {mmf_name}")
    except Exception as e:
        print("Open shared mem failed:", e)
        return -1

    # Read header/footer
    g_hMap.seek(0)
    header = struct.unpack_from("<Q", g_hMap, 0)[0]   # __int64
    footer = struct.unpack_from("<Q", g_hMap, MMF_DATA_SIZE - 8)[0]

    # If header/footer are incorrect, reset (simulate C++ behavior)
    if header != MMF_DATA_HEADER or footer != MMF_DATA_FOOTER:
        # Python can also write back directly
        g_hMap.seek(0)
        g_hMap.write(struct.pack("<Q", MMF_DATA_HEADER))  # header
        g_hMap.seek(MMF_DATA_SIZE - 8)
        g_hMap.write(struct.pack("<Q", MMF_DATA_FOOTER))  # footer
        print("Reset shared mem header/footer")
        return 0

    # Read image_status
    g_hMap.seek(8)
    image_status = struct.unpack("<i", g_hMap.read(4))[0]

    if image_status == 1:
        # Read metadata
        g_hMap.seek(12)
        image_width, image_height, image_size = struct.unpack("<III", g_hMap.read(12))
        timestamp = struct.unpack("<Q", g_hMap.read(8))[0]

        # Read image_data
        g_hMap.seek(32)
        image_data = g_hMap.read(image_size)

        # Write back image_status = 2
        g_hMap.seek(8)
        g_hMap.write(struct.pack("<i", 2))

        # Return values simulate C++ pointer/reference
        frame_holder[:] = [bytes(image_data)]
        width_holder[:] = [image_width]
        height_holder[:] = [image_height]
        size_holder[:] = [image_size]
        timestamp_holder[:] = [timestamp]

        return 1
    return 0

# ---------- Background Thread ----------

def RecognizeTask():
    print("start get shared mem thread")
    count = 0

    while True:
        if not g_running:
            break

        frame = []
        width = []
        height = []
        size = []
        timestamp = []
        if get_mmf(frame, width, height, size, timestamp) == 1:
            if g_isSetting and size[0] > 0:                
                # Use pluggable detector instead of simulation
                detections = []
                try:
                    if g_detector is not None:
                        # Pass ROI rectangles for filtering if available
                        detections = g_detector.detect(frame[0], width[0], height[0], g_roi_rects if g_roi_rects else None)
                except Exception as det_e:
                    print(f"[Detector] error: {det_e}")
                    detections = []

                if detections and g_callbackFunction:
                    # Build ROI groups from detections (single row of detections)
                    roi_row = [ROI(int(x), int(y)) for (x, y, w, h) in detections]
                    rois = [roi_row]
                    rows = 1
                    cols = len(roi_row)

                    g_callbackFunction(
                        g_portnum,
                        width[0],
                        height[0],
                        frame[0],         # bytes
                        size[0],
                        timestamp[0],
                        rois,             # grouped ROIs (1 x N)
                        rows,
                        cols,
                        detections        # Pass full detections for drawing boxes
                    )

                count += 1

        # sleep
        time.sleep(0.005)

    print("exit get shared mem thread")



# ---------- API ----------
def Initialize(PortNumber: int):
    global g_portnum, g_bgThread, g_running, g_detector
    g_portnum = PortNumber
    g_running = True
    g_detector = get_default_detector()
    # Set default confidence threshold (will be overridden by SettingParameters if provided)
    if g_detector:
        g_detector.set_confidence_threshold(0.25)  # Default permissive confidence
    print(f"DLL Initialized, Port ID = {g_portnum}")
    g_bgThread = threading.Thread(target=RecognizeTask, daemon=True)
    g_bgThread.start()


def SettingParameters(parameters: SettingParameters):
    print("analytics_engine SettingParameters")
    global g_url, g_isSetting, g_detector, g_roi_rects
    
    g_url = parameters.analytics_event_api_url
    print("Parameters set:")
    print("version:", parameters.version)
    print("analytics_event_api_url:", g_url)
    print("image_width:", parameters.image_width)
    print("image_height:", parameters.image_height)
    print("jpg_compress:", parameters.jpg_compress)
    
    # Process ROI groups and extract threshold/sensitivity settings
    g_roi_rects = []
    active_threshold = -1
    active_sensitivity = -1
    
    print(f"Processing {len(parameters.rois)} ROI groups:")
    for i, roi_group in enumerate(parameters.rois):
        print(f"ROI Group {i}: sensitivity={roi_group.sensitivity}, threshold={roi_group.threshold}, {len(roi_group.rects)} points")
        
        # Use first valid threshold/sensitivity pair for detector
        if active_threshold == -1 and roi_group.threshold > 0 and roi_group.sensitivity > 0:
            active_threshold = roi_group.threshold
            active_sensitivity = roi_group.sensitivity
            print(f"Using threshold={active_threshold}, sensitivity={active_sensitivity} for detector")
        
        # Convert 4 corner points to rectangle
        if len(roi_group.rects) >= 4:
            # Take first 4 points and find bounding rectangle
            xs = [point.x for point in roi_group.rects[:4]]
            ys = [point.y for point in roi_group.rects[:4]]
            
            x1, x2 = min(xs), max(xs)
            y1, y2 = min(ys), max(ys)
            
            g_roi_rects.append((x1, y1, x2, y2))
            print(f"  Created ROI rectangle from 4 points: ({x1}, {y1}, {x2}, {y2})")
            
            # Print all 4 corner points for debugging
            for j, point in enumerate(roi_group.rects[:4]):
                print(f"    Point {j}: ({point.x}, {point.y})")
                
        elif len(roi_group.rects) == 2:
            # Fallback: 2 points define diagonal corners
            x1, y1 = roi_group.rects[0].x, roi_group.rects[0].y
            x2, y2 = roi_group.rects[1].x, roi_group.rects[1].y
            # Ensure x1,y1 is top-left and x2,y2 is bottom-right
            x1, x2 = min(x1, x2), max(x1, x2)
            y1, y2 = min(y1, y2), max(y1, y2)
            g_roi_rects.append((x1, y1, x2, y2))
            print(f"  Created ROI rectangle from 2 points: ({x1}, {y1}, {x2}, {y2})")
        elif len(roi_group.rects) > 0:
            print(f"  Warning: {len(roi_group.rects)} points provided for ROI group {i}, need 2 or 4 points to form rectangle")
    
    # Convert threshold and sensitivity to YOLO confidence if we have valid values
    if active_threshold > 0 and active_sensitivity > 0 and g_detector:
        from detectors import convert_threshold_to_confidence
        confidence = convert_threshold_to_confidence(active_threshold, active_sensitivity)
        g_detector.set_confidence_threshold(confidence)
        print(f"Set detector confidence threshold: {confidence} (from threshold={active_threshold}, sensitivity={active_sensitivity})")
    
    if g_roi_rects:
        print(f"Total ROI rectangles configured: {len(g_roi_rects)}")
    else:
        print("No ROI filtering configured - all detections will be reported")
    
    g_isSetting = True


def registerCallback(callback):
    global g_callbackFunction
    g_callbackFunction = callback


def unregisterCallback():
    global g_callbackFunction
    g_callbackFunction = None


def Deinitialize():
    global g_running, g_bgThread
    print("DLL Deinitialized")
    g_running = False
    if g_bgThread:
        g_bgThread.join()

# Allow swapping detector at runtime

def set_detector(detector: BaseDetector):
    global g_detector
    g_detector = detector
    print(f"Detector set to: {type(detector).__name__}")
