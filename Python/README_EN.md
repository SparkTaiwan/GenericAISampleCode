# SampleWrapper Python Version

```text
python_version/
├── analytics_engine.py     # Analytics engine (corresponds to C++ DLL functionality)
├── data_structures.py      # Data structure definitions (corresponds to C# structures)
├── detectors.py            # Detection module (YOLO human detection)
├── http_client.py          # HTTP client (corresponds to SimpleHttpClient)
├── http_server.py          # HTTP server (corresponds to SimpleHttpServer)
├── image_processor.py      # Image processing module (YUV420 conversion etc.)
├── main.py                 # Main program (corresponds to Program.cs)
├── test_yolo_detector.py   # YOLO detector test
├── build.bat               # Windows build script
├── SampleWrapper.spec      # PyInstaller configuration file (optimized)
├── requirements.txt        # Python dependencies
├── yolov8n.pt             # YOLO model file
└── README.md              # Documentation
```

This is a Python reimplementation of the original C#/.NET and C++ DLL project, using YOLO for human detection.

## Project Structure

```text
python_version/
├── analytics_engine.py     # Analytics engine (corresponds to C++ DLL functionality)
├── data_structures.py      # Data structure definitions (corresponds to C# structures)
├── detectors.py            # Detection module (YOLO human detection)
├── http_client.py          # HTTP client (corresponds to SimpleHttpClient)
├── http_server.py          # HTTP server (corresponds to SimpleHttpServer)
├── image_processor.py      # Image processing module (YUV420 conversion etc.)
├── main.py                 # Main program (corresponds to Program.cs)
├── test_yolo_detector.py   # YOLO detector test
├── build.bat               # Windows build script
├── SampleWrapper.spec      # PyInstaller configuration file (optimized)
├── requirements.txt        # Python dependencies
├── yolov8n.pt             # YOLO model file
└── README.md              # Documentation
```

## Feature Mapping

### Original C# Project Features

- **Program.cs**: Main program logic, DLL calls, callback handling
- **SimpleHttpClient.cs**: HTTP client functionality
- **SimpleHttpServer.cs**: HTTP server functionality
- **C++ DLL**: Shared memory handling, image analysis, callback mechanism

### Python Reimplementation (Lightweight Architecture)

- **main.py**: Equivalent to Program.cs, handles main logic and callbacks
- **analytics_engine.py**: Equivalent to C++ DLL, handles shared memory simulation and detection callbacks
- **http_client.py**: Equivalent to SimpleHttpClient.cs, sends detection results
- **http_server.py**: Equivalent to SimpleHttpServer.cs, receives configuration parameters
- **detectors.py**: YOLO-based human detection (replaces traditional computer vision)
- **data_structures.py**: Python dataclasses equivalent to C# structures
- **image_processor.py**: YUV420 to JPEG conversion and image processing

## Quick Start

### 1. Install Dependencies

```bash
# Install Python dependencies
pip install -r requirements.txt

# Download YOLO model (if not included)
# The yolov8n.pt file should be in the project directory
```

### 2. Run the Application

```bash
# Start with default port (51000)
python main.py

# Start with custom port
python main.py port=52000

# Enable debug mode (saves detection images)
python main.py port=51000 debug

# Specify shared memory port separately
python main.py port=51000 shm_port=51001 debug=true
```

### 3. Configure Detection Parameters

Use POST request to set detection parameters:

```bash
curl -X POST http://127.0.0.1:51000/SetParameters \
  -H "Content-Type: application/json" \
  -d '{
    "version": "1.2",
    "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
    "image_width": 1280,
    "image_height": 720,
    "jpg_compress": 75,
    "rois": [
      {
        "sensitivity": 70,
        "threshold": 30,
        "rects": [
          {"x": 8, "y": 8},
          {"x": 8, "y": 717},
          {"x": 1277, "y": 8},
          {"x": 1277, "y": 717}
        ]
      }
    ]
  }'
```

### 4. Check Service Status

```bash
# Health check
curl http://127.0.0.1:51000/Alive

# License check
curl http://127.0.0.1:51000/GetLicense
```

## How It Works

1. **Initialization**: Start HTTP server and shared memory monitoring
2. **Parameter Setup**: Receive detection parameters via HTTP API
3. **Image Processing**: Read YUV420 frames from shared memory
4. **Human Detection**: Use YOLO model to detect humans in frames
5. **ROI Filtering**: Filter detections to only include specified regions
6. **Result Transmission**: Send detection results with Base64 JPEG to target server

Key Features:
- Real-time human detection using state-of-the-art YOLO models
- ROI (Region of Interest) filtering to reduce false positives
- Dynamic threshold adjustment via API
- Debug mode with automatic image saving
- Detection results are sent to the specified analytics_event_api_url

## Build Instructions

### Quick Build

```bash
# Use the build script
./build.bat

# Or build manually
pip install pyinstaller
pyinstaller SampleWrapper.spec
```

This creates a standalone executable in the `dist` folder.

### Executable Usage

```bash
# Run the built executable
cd dist
SampleWrapper.exe port=51000

# With debug mode
SampleWrapper.exe port=51000 debug
```

## YOLO Detection

### Model Information

- **Model**: YOLOv8n (nano version for speed)
- **Classes**: Detects 80 COCO classes including 'person'
- **Input**: 640x640 RGB images (auto-scaled from YUV420)
- **Output**: Bounding boxes with confidence scores

### Detection Process

1. **Frame Conversion**: YUV420 → RGB → 640x640 tensor
2. **Model Inference**: YOLO processes the frame
3. **Post-processing**: Filter for 'person' class and apply confidence threshold
4. **ROI Filtering**: Only keep detections within specified regions
5. **Coordinate Mapping**: Scale detection coordinates back to original image size

### Confidence Threshold

The system converts user-friendly sensitivity/threshold parameters to YOLO confidence:

```python
def convert_threshold_to_confidence(threshold, sensitivity):
    # Inverse relationship: higher threshold = higher confidence requirement
    # Lower sensitivity = higher confidence requirement
    normalized_threshold = threshold / 100.0
    normalized_sensitivity = sensitivity / 100.0
    
    # Base confidence starts high and decreases with lower threshold and higher sensitivity
    confidence = 0.5 + (normalized_threshold * 0.3) - (normalized_sensitivity * 0.4)
    return max(0.1, min(0.9, confidence))
```

## ROI (Region of Interest) Filtering

The system supports multiple rectangular regions for detection filtering:

### ROI Configuration
- Each ROI group contains sensitivity, threshold, and rectangle coordinates
- Rectangle defined by 4 corner points (any order)
- Only detections within ROI regions will trigger alerts
- If no ROI is set, the entire frame will be monitored
- Backward compatible: also supports 2-point diagonal definition

## API Endpoints

### POST /SetParameters

Set analysis parameters, JSON format:

```json
{
  "version": "1.2",
  "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
  "image_width": 1280,
  "image_height": 720,
  "jpg_compress": 75,
  "rois": [
    {
      "sensitivity": 50,
      "threshold": 50,
      "rects": [
        {"x": 8, "y": 8},
        {"x": 8, "y": 717},
        {"x": 1277, "y": 8},
        {"x": 1277, "y": 717}
      ]
    }
  ]
}
```

**Parameter Description**:

- `version`: API version (fixed at "1.2")
- `analytics_event_api_url`: Detection result receiver server URL
- `image_width` / `image_height`: Image dimensions
- `jpg_compress`: JPEG compression quality (1-100)
- `rois`: ROI region array, each region contains:
  - `sensitivity` (0-100): Detection sensitivity, higher values make detection easier
  - `threshold` (0-100): Detection threshold, higher values make detection stricter
  - `rects`: 4 corner point coordinates defining the rectangular detection area

**Threshold Conversion Examples**:

- `sensitivity=80, threshold=20` → YOLO confidence ≈ 16% (very loose)
- `sensitivity=50, threshold=50` → YOLO confidence ≈ 32% (medium)
- `sensitivity=20, threshold=80` → YOLO confidence ≈ 49% (strict)

### GET /Alive

Health check endpoint

### GET /GetLicense

License check endpoint

## Detection Result Response Format

When humans are detected, the system sends detection results to the configured `analytics_event_api_url`.

### Response Structure

```json
{
  "version": "1.2",
  "port_num": 1,
  "keyframe": "/9j/4AAQSkZJR...",
  "timestamp": 15003215760000,
  "rois_rects": [
    [
      {"x": 0, "y": 0},
      {"x": 10, "y": 0}, 
      {"x": 10, "y": 10},
      {"x": 0, "y": 10}
    ],
    [
      {"x": 50, "y": 50},
      {"x": 60, "y": 50},
      {"x": 60, "y": 60},
      {"x": 50, "y": 60}
    ]
  ]
}
```

### Parameter Description

#### Top-level Parameters

| Name | Type | Example | Description |
|------|------|---------|-------------|
| version | string | "1.2" | Data format version for compatibility tracking |
| port_num | int | 1 | Video input port or channel identifier |
| keyframe | string (Base64) | "/9j/4AAQSkZJR..." | Keyframe image encoded in Base64, usually in JPEG format |
| timestamp | int (epoch-based) | 15003215760000 | Frame timestamp. Unit can be milliseconds or microseconds depending on the system |

#### rois_rects Array

The `rois_rects` parameter is an array containing multiple rectangular ROIs. Each ROI is defined by four corner points.

**ROI Rectangle Example**:

```json
[
  {"x": 0, "y": 0},   // Top-left
  {"x": 10, "y": 0},  // Top-right
  {"x": 10, "y": 10}, // Bottom-right
  {"x": 0, "y": 10}   // Bottom-left
]
```

**Important Notes**:

- `keyframe` is a JPEG format image encoded as a Base64 string
- `version` is used to define the version number of the transmitted JSON
- Each detected object is represented by 4 corner points of its bounding box
- Coordinate system origin (0,0) is at the top-left corner of the image

## Key Differences

### Differences from Original C# Version

1. **Asynchronous Processing**: Uses `asyncio` instead of C# `async/await`
2. **HTTP Framework**: Uses built-in `http.server` instead of `HttpListener` (lightweight implementation)
3. **HTTP Client**: Uses `requests` instead of `HttpClient`
4. **Shared Memory**: Uses file mapping to simulate Windows named shared memory
5. **Image Processing**: Uses `numpy` and `PIL` instead of `System.Drawing`
6. **Human Detection**: Uses YOLO (ultralytics) for human detection
7. **Parameter Format**: Supports more flexible JSON parameter format
8. **ROI Filtering**: Intelligent region filtering, only detects humans in specified areas
9. **Dynamic Threshold**: Can dynamically adjust detection sensitivity and threshold via API

### Differences from Original C++ DLL

1. **Thread Management**: Uses Python `threading` instead of C++ `std::thread`
2. **Memory Management**: Python automatic garbage collection, no manual management needed
3. **Platform Compatibility**: Can run on multiple platforms, not limited to Windows
4. **Detection Algorithm**: Uses modern deep learning models (YOLO) instead of traditional computer vision methods
5. **Real-time Adjustment**: Supports runtime dynamic adjustment of detection parameters without restart
6. **Detailed Logging**: Provides rich detection logs and debugging information

### New Features

1. **Intelligent ROI Filtering**:
   - Supports multiple rectangular region definitions
   - Only detections within specified regions trigger alerts
   - Significantly reduces false positive rates

2. **Dynamic Threshold Conversion**:
   - Automatically converts user-friendly sensitivity/threshold parameters to YOLO confidence
   - Supports real-time adjustment without model retraining

3. **Enhanced Debug Mode**:
   - Automatically saves detection result images
   - Detailed detection process logs
   - Transparent threshold conversion process

4. **Flexible Configuration**:
   - JSON-based parameter configuration
   - Support for multiple ROI groups with different sensitivities
   - Runtime parameter adjustment via HTTP API

5. **Modern Architecture**:
   - Async/await pattern for better performance
   - Modular design for easy maintenance
   - Pluggable detector architecture for future extensions

## Troubleshooting

### Common Issues

1. **Service won't start**:
   - Check if port is already in use: `netstat -an | findstr :51000`
   - Try using a different port: `python main.py port=52000`
   - Run with administrator privileges if needed

2. **No detection results**:
   - Verify parameters are set correctly via POST /SetParameters
   - Check if shared memory contains image data
   - Enable debug mode to see detailed logs: `python main.py debug`
   - Verify YOLO model file exists: `yolov8n.pt`

3. **ROI filtering not working**:
   - Verify JSON format is correct with proper ROI group structure
   - Each ROI group's rects array should contain 4 corner point coordinates
   - Check coordinate values are within image bounds
   - Use debug mode to confirm ROI rectangle creation success

4. **Parameter setting not effective**:
   - Confirm POST /SetParameters request returns success response
   - Check JSON format is correct
   - Verify server has received and processed parameters

5. **Performance issues**:
   - Reduce JPEG compression quality to speed up processing
   - Adjust detection confidence threshold
   - Consider using smaller ROI regions
   - Monitor CPU and memory usage

### Debug Mode

Enable debug mode for detailed troubleshooting:

```bash
python main.py port=51000 debug
```

Debug mode provides:
- Detailed detection process logs
- Automatic saving of detection images with bounding boxes
- ROI rectangle visualization
- Threshold conversion information
- Shared memory data status

### Log Analysis

The application provides comprehensive logging:

```
[LOG] Waiting for valid parameter settings...
[LOG] Received valid parameter settings!
[LOG] System is fully ready!
[DEBUG] Detection callback triggered - received shared memory data
[DEBUG] Sending rois_rects: [...]
```

Monitor these logs to understand the system state and identify issues.

## License

This project is provided as-is for demonstration purposes. For production use, ensure you have appropriate licenses for:

- YOLO model usage (Ultralytics license)
- Any third-party dependencies
- Commercial deployment requirements

## Support

For issues and questions:

1. Check this documentation first
2. Review debug logs for error messages
3. Verify system requirements are met
4. Test with minimal configuration first