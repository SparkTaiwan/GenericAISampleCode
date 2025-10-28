# CSharp Sample Code

This folder contains C# sample code for AI integration, demonstrating how to wrap AI modules and communicate with NVR/CMS systems using .NET.

## Project Structure

- **Program.cs**: Main program logic, including DLL invocation, callback handling, and coordination between components.
- **SimpleHttpClient.cs**: HTTP client implementation using HttpClient for sending analytics results to external APIs.
- **SimpleHttpServer.cs**: HTTP server implementation using HttpListener for receiving parameters, health checks, and license validation.
- **SampleDLL/**: C++ DLL project providing shared memory handling, image analysis, and callback mechanisms.
  - **dllmain.cpp**: Main DLL entry point with shared memory structures and processing logic.
  - **framework.h**, **pch.cpp**, **pch.h**: Standard C++ project files.
  - **SampleDLL.vcxproj**: Visual C++ project file.
- **SampleWrapper.csproj**: C# project file defining dependencies and build settings.
- **SampleWrapper.sln**: Visual Studio solution file (developed using VS2022).
- **packages.config**: NuGet package dependencies (e.g., Newtonsoft.Json for JSON handling).
- **App.config**: Application configuration file.

## Prerequisites

- Visual Studio 2022 with .NET Framework support.
- Windows OS (required for HttpListener and shared memory features).

## Building and Running

1. Open `SampleWrapper.sln` in Visual Studio 2022.
2. Restore NuGet packages (right-click solution > Restore NuGet Packages).
3. Build the solution (Build > Build Solution).
4. Run the project (F5 or Debug > Start Debugging).

The application will start an HTTP server on the default port (configurable) and wait for parameter settings and image data via shared memory.

## Key Features

- **HTTP Communication**: JSON-based API for parameter configuration and health checks.
- **Shared Memory Integration**: Uses Windows shared memory for efficient image data transfer.
- **DLL Integration**: Calls C++ DLL for image processing and analytics.
- **Callback Mechanism**: Handles asynchronous callbacks from the DLL for detection results.

## API Endpoints

- `POST /SetParameters`: Set analysis parameters (JSON payload).
- `GET /Alive`: Health check.
- `GET /GetLicense`: License validation.

## Dependencies

- .NET Framework 4.x
- Newtonsoft.Json (for JSON serialization)
- Windows APIs (for shared memory and HTTP listener)

## Notes

This C# implementation serves as a reference for integrating AI modules with existing NVR/CMS systems. The C++ DLL handles the core analytics, while the C# wrapper manages communication and configuration.
