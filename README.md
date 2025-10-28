
# GenericAISampleCode

A generic AI integration sample project. This repository demonstrates how to build a cross-language AI interface using HTTP/JSON for communication, enabling integration between NVR/CMS systems and various AI modules.

## Project Structure

### CSharp/

Contains C# sample code for AI integration. This folder includes:

- **Program.cs**: Main program logic, DLL invocation, and callback handling.
- **SimpleHttpClient.cs**: HTTP client implementation for sending analytics results.
- **SimpleHttpServer.cs**: HTTP server implementation for receiving parameters and health checks.
- **SampleDLL/**: C++ DLL project for shared memory handling, image analysis, and callback mechanisms.
- **SampleWrapper.csproj**: C# project file.
- **SampleWrapper.sln**: Visual Studio solution file (developed using VS2022).

The C# version demonstrates how to wrap AI modules and communicate with NVR/CMS systems using .NET.

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

Contains test programs and related files for integration testing. This includes executable files and configuration files for testing the AI integration. The source code for this part will be organized and uploaded in future updates.

### Documentation

- **SparkARGO-ARGO Generic AI Integration Sample Code Guide Version 1.2.pdf**: English guide providing an overview of the architecture and design principles.
- **SparkARGO-Argo 通用 AI 整合文件 version 1.2.pdf**: Chinese documentation with similar content.

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

- The repository currently provides sample code for C# and Python. You may choose either language as a starting point for your integration.
- Test program source code will be organized and uploaded in future updates.
- For details on the architecture and design, please refer to the provided PDF documents.

## License

This project is licensed under the Apache License 2.0.
