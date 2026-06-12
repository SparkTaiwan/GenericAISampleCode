
# GenericAISampleCode

A generic AI integration sample project. This repository demonstrates how to build a cross-language AI interface using HTTP/JSON for control and shared memory for frame transfer, enabling integration between NVR/CMS systems and various AI modules. Besides the reference samples (C#, Python) it also hosts the production multi-channel wrapper (`GenericAI/`).

## Project Structure

### GenericAI/

The production multi-channel AI wrapper that replaces the single-channel `CSharp/` sample: a C# host process (`GenericAI.exe`) plus a C++ detector DLL (`GenericAI.Native.dll`, Motion or YOLOX person detection) serving N video channels from one process. See [GenericAI/README.md](GenericAI/README.md) (English) or [GenericAI/README_zhTW.md](GenericAI/README_zhTW.md) (繁體中文).

### CSharp/

The original single-channel C# sample wrapper (`SampleWrapper.exe`), kept as reference code. This folder includes:

- **Program.cs**: Main program logic, DLL invocation, and callback handling.
- **SimpleHttpClient.cs** / **SimpleHttpServer.cs**: HTTP client / server for analytics results, parameters, and health checks.
- **FileLogger.cs** / **TurboJpegInterop.cs**: file logging and libjpeg-turbo JPEG encoding.
- **SampleDLL/**: C++ DLL project for shared memory handling, image analysis, and callback mechanisms.
- **SampleWrapper.sln** / **SampleWrapper.csproj**: Visual Studio solution and project files.

The C# version demonstrates how to wrap AI modules and communicate with NVR/CMS systems using .NET. See [CSharp/README.md](CSharp/README.md).

### CSharp_Single/

A variant of the C# sample exercising the v1.3 "Single mode" protocol, where one process opens multiple channels through a single DLL instead of one process per channel.

### Python/

Contains Python sample code, providing a re-implementation of the main logic with modern libraries. Key files include:

- **analytics_engine.py**: Analytics engine (corresponding to C++ DLL functionality).
- **data_structures.py**: Data structure definitions (corresponding to C# structs).
- **detectors.py**: Detection module using YOLO for human detection.
- **http_client.py**: Lightweight HTTP client (using requests library).
- **http_server.py**: Lightweight HTTP server (using built-in http.server).
- **image_processor.py**: Image processing module for YUV420 to RGB/JPEG conversion.
- **main.py**: Main program (corresponding to Program.cs).
- **test_yolo_detector.py**: YOLO detector test script.
- **requirements.txt**: Python dependencies.
- **yolov8n.pt**: YOLO model file.
- **build.bat**: Windows build script for creating executable.
- **SampleWrapper.spec**: PyInstaller configuration for packaging.

The Python version is designed to be lightweight, cross-platform, and easy to extend, using YOLO for human detection instead of traditional computer vision methods.

### TestProg/

Contains the prebuilt test program (`Spark_Test_Prog.exe` with its OpenCV / FFmpeg runtime and `param.json`) for integration testing. The source code for this part will be organized and uploaded in future updates.

### Documentation

- **SparkARGO-ARGO Generic AI Integration Sample Code Guide Version 1.2.pdf**: English guide providing an overview of the architecture and design principles.

## Architecture (textual)

The following is a text-first representation of the runtime architecture and data flow between the test program (represented by `Spark_Test_Prog.exe` — "ARGO") and the `SampleWrapper.exe` wrapper. It reproduces the diagram information in plain text so it is visible in any viewer.

Textual diagram (left = ARGO / Spark_Test_Prog.exe, right = SampleWrapper.exe):

```text
ARGO / Spark_Test_Prog.exe                   SampleWrapper.exe
+---------------------------+                  +---------------------------+
| Http Client               | ---(1) ----->    | Http Server               |
| - Send Alive              |                  | - Receive Alive           |
| - Send SetParameters      |                  | - Receive SetParameters   |
| - Send License Check      | <---(1a)----     | - Receive License Check   |
+---------------------------+                  +---------------------------+

+---------------------------+                  +---------------------------+
| Http Server               | <---(2)----      | Http Client               |
| - Receive Analytics event |                  | - Post analytics result   |
|   (Post from SampleWrap)  | ---(2a)---->     |   to ARGO                 |
+---------------------------+                  +---------------------------+

+---------------------------+                  +---------------------------+
| Shared memory (writer)    | ===(3) frame===>  | Shared memory (reader)    |
| - Writes frame bytes      |                  | - Reads frame bytes       |
+---------------------------+                  +---------------------------+
```

Detailed sequence and data flows (numbered):

1) Control / configuration flow (HTTP):
   - `Spark_Test_Prog.exe` (ARGO) acts as an HTTP client and sends control messages to `SampleWrapper.exe` HTTP server:
     - Send Alive (health check)
     - Send `SetParameters` (analytics configuration JSON)
     - Send license check requests
   - `SampleWrapper.exe` responds with HTTP status (OK/400/etc.).

2) Analytics result flow (HTTP):
   - After analyzing frames, `SampleWrapper.exe` posts analytics results to ARGO's HTTP server (example endpoint: `/PostAnalyticsResult`).
   - ARGO responds with a confirmation (HTTP OK) upon receipt.

3) Frame / image flow (shared memory):
   - ARGO writes raw frame/image bytes into a named shared memory segment (includes header fields like status, width, height, timestamp, size).
   - `SampleWrapper.exe` reads frames from that shared memory for analysis.

Common endpoints and responsibilities:
- `POST /SetParameters` — set analytics parameters (JSON payload).
- `GET /Alive` — health check from ARGO to SampleWrapper.
- `GET /GetLicense` — license validation/request.
- `POST /PostAnalyticsResult` — endpoint on ARGO to receive analytics/detection results.

Implementation notes:
- Shared memory is used for high-throughput frame transfer; control and result messages use HTTP/JSON.
- The C# examples define shared-memory layout (`MMF_Data` struct) and HTTP handlers; the Python example follows the same high-level contract.
- Separating control (HTTP) and frame transfer (shared memory) reduces IPC overhead and keeps the protocol simple.

## SampleWrapper runtime flow

This section describes the runtime behavior and internal processing steps of `SampleWrapper.exe` (the wrapper that performs analytics). Follow these steps to understand what the wrapper does after it starts.

1. Start HTTP server
   - On startup, `SampleWrapper.exe` creates and starts an HTTP server (port configurable via CLI or config). The server exposes endpoints for control and status (for example, `/Alive`, `/SetParameters`, `/GetLicense`) and returns appropriate HTTP responses.

2. Receive `SetParameters`
   - When `POST /SetParameters` is received, the server parses the JSON payload and extracts:
     a. Supported version number(s)
     b. Post URL for analytics results (`analytics_event_api_url`)
     c. Image width and height (`image_width`, `image_height`)
     d. ROI and other detection-related settings (`sensitivity`, `threshold`, `rois`, `jpg_compress`, etc.)

3. Create shared memory
   - Using the information from `SetParameters` (image size, frame size), the wrapper creates or opens the named shared-memory segment that will be used to read frames. It sets up any internal buffers and state needed to read frame headers (status flags, timestamps, sizes).

4. Read frames from shared memory and run detection
   - The wrapper continuously polls or waits on the shared-memory segment to find new frames (status flag indicating a fresh frame).
   - When a new frame is available, it reads the frame bytes (using the agreed header format), converts/decodes if necessary, and passes the frame into the detection pipeline (YOLO model, native DLL, or other analyzer).

5. Post analytics results to ARGO
   - After inference, the wrapper formats the detection result into the agreed JSON schema (versioned) and POSTs it to the `analytics_event_api_url` provided in step 2 (ARGO's HTTP endpoint).
   - It expects an HTTP OK response and may retry or log errors on failure.

Continuous operation and control
   - Steps 4 and 5 repeat continuously while the wrapper is running; the wrapper also concurrently accepts and responds to `Alive` and `GetLicense` requests at any time.
   - `Alive` should return a simple status (OK + optional metadata). `GetLicense` should validate or return license status per the implementation.

Notes and implementation tips
   - The shared-memory header (see `MMF_Data` in `CSharp/SampleDLL/dllmain.cpp`) contains useful fields (header/footer markers, status, `image_width`/`image_height`, `image_size`, `timestamp`) — follow the same layout for cross-language compatibility.
   - Keep HTTP control logic separate from the frame read / detection loop (use separate threads or async tasks) so control endpoints remain responsive.
   - Implement exponential backoff or retry logic when posting results to ARGO to handle transient network errors.

## Getting Started

### Prerequisites

- For C#: Visual Studio 2022 with .NET Framework support.
- For Python: Python 3.8+, pip for package management.

### Running the Python Version

1. Install dependencies:

   ```bash
   pip install -r Python/requirements.txt
   ```

2. Run the main program:

   ```bash
   python Python/main.py -port=51000
   ```

   For debug mode:

   ```bash
   python Python/main.py -port=51000 debug
   ```

3. Set parameters via API:

   ```bash
   curl -X POST http://127.0.0.1:51000/SetParameters -H "Content-Type: application/json" -d '{"version": "1.2", "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult", "image_width": 1280, "image_height": 720, "jpg_compress": 75, "rois": [{"sensitivity": 50, "threshold": 50, "rects": [{"x": 8, "y": 8}, {"x": 8, "y": 717}, {"x": 1277, "y": 8}, {"x": 1277, "y": 717}]}]}'
   ```

### Building the Python Version

Use the provided build script:

```bash
Python/build.bat
```

This creates a standalone executable in the `dist` directory.

## Notes

- The repository provides sample code for C# and Python — you may choose either language as a starting point for your integration — plus the production-grade `GenericAI/` wrapper.
- Test program source code will be organized and uploaded in future updates.
- For details on the architecture and design, please refer to the provided PDF document.

## License

This project is licensed under the Apache License 2.0.
