# GenericAI

Multi-channel AI wrapper that companions Spark Recorder's `AStreamPerimeter_GenericAI` AI-service module. One process serves N independent video channels sharing a single detector backend.

繁體中文版：see [README_zhTW.md](README_zhTW.md).

## Components

| Project | Type | Output | Purpose |
| --- | --- | --- | --- |
| `GenericAI.App` | C# (.NET Framework 4.8, WPF-less) | `GenericAI.App.exe` | Host process: HTTP listeners, MMF readers, JPEG encode + HTTP send worker pools, native interop. |
| `GenericAI.Native` | C++ (MSVC v140) | `GenericAI.Native.dll` | Detector implementations + per-channel pipeline. Exports `GAI_*` C ABI. |

## Requirements

- Windows 10 / 11, x64.
- Visual Studio 2019 or 2022 with the **MSVC v140 toolset** and **Windows 8.1 SDK** (the C++ project targets v140 to match Spark Recorder's link environment).
- .NET Framework **4.8** Developer Pack.
- DirectML-capable GPU recommended for the person detector; the loader falls back to CPU automatically.

Third-party dependencies (restored via NuGet):
- `Microsoft.ML.OnnxRuntime.DirectML` 1.14.1 — person detector inference.
- `Microsoft.AI.DirectML` 1.10.1.
- `Newtonsoft.Json` 13.0.3 — HTTP `POST /SetParameters` payload.
- `libjpeg-turbo` (bundled at `native-deps/win-x64/turbojpeg.dll`) — callback JPEG encoding.

The person detector model is bundled at `native-deps/models/yolox_m_fp16.onnx` (YOLOX-M, FP16). The path is resolved relative to the exe.

## Build

```powershell
# from the repo root
nuget restore GenericAI/GenericAI.sln
msbuild GenericAI/GenericAI.sln /p:Configuration=Release /p:Platform=x64
```

Or open `GenericAI/GenericAI.sln` in Visual Studio, pick `x64 / Release`, and build. Output lands in `GenericAI/bin/Release/`.

## Run

```
GenericAI.App.exe [port=<X>] [channel_count=<N>] [encode_workers=<E>] [send_workers=<S>] [log=<dir>]
```

| Argument | Default | Meaning |
| --- | --- | --- |
| `port` | `51000` | Base sample / callback port. Channel `k` (0-indexed) listens on `port + 2*k`. |
| `channel_count` | `1` | Number of channels served by this process. No upper bound; `<1` is rejected. |
| `encode_workers` | `2` | JPEG-encode worker threads (process-wide pool, shared across channels). |
| `send_workers` | `2` | HTTP-POST worker threads (process-wide pool). |
| `log` | `""` (auto) | Override the log directory. Defaults to `D:\SLog-<basePort>\GenericAI.log`. |

Exit codes:

| Code | Meaning |
| --- | --- |
| `0` | OK |
| `1` | Generic failure |
| `2` | Bad arguments |
| `3` | Port already in use |
| `4` | Native init failed (detector / ONNX session / MMF) |

Production-side spawn always passes `port=` explicitly. Running the exe without args picks `port=51000` ("Debug Run" mode) so you can press F5 from Visual Studio.

## Wire Protocol

The HTTP control plane and MMF layout match `spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp` exactly so the recorder side stays unchanged.

### HTTP (recorder ↔ wrapper)

Each channel listens on its own port:

| Endpoint | Direction | Purpose |
| --- | --- | --- |
| `GET /Alive` | recorder → wrapper | Health probe, returns 200 OK. |
| `GET /GetLicense` | recorder → wrapper | License token endpoint. |
| `POST /SetParameters` | recorder → wrapper | v1.2 JSON: `{analytics_event_api_url, image_width, image_height, jpg_compress, rois: [{sensitivity, threshold, rects: [{x,y}, ...]}]}`. |
| `POST <analytics_event_api_url>` | wrapper → recorder | Detection callback, body = JPEG of the keyframe + ROI metadata. |

### MMF (recorder → wrapper)

- Per-channel name: `ChannelFrame_<channelPort>` (channel `k` reads from `ChannelFrame_{port + 2*k}`).
- Layout mirrors `MMF_Data` in the legacy `CSharp/SampleDLL/dllmain.cpp` to keep the recorder writer unchanged.
- `status` byte: `0=unused → 1=new frame → 2=consumed`. The wrapper polls and flips `1 → 2` after acquiring a frame.

## Detector Backends

Selected at compile time via `gai_config.h`:

- **Person** — YOLOX-M FP16 ONNX, DirectML EP with CPU fallback. Letterbox preprocess + NMS + ROI overlap filter. Per-channel ROI list comes from `POST /SetParameters`.
- **Motion** — frame-diff with per-pixel threshold, fused per-ROI loop with early exit. Same configuration knobs as the legacy `CSharp/SampleDLL` motion detector.

A single detector instance is shared across all channels in the process via `SharedDetectorScheduler`; channels round-robin through it and dispatch results back to the originating channel through `FrameDispatcher`.

## Relationship to `CSharp/`

`CSharp/` is the legacy single-channel sample wrapper (`SampleWrapper.exe` + `SampleDLL.dll`) preserved as reference code. GenericAI replaces it for production use:

| | `CSharp/SampleWrapper.exe` | `GenericAI.App.exe` |
| --- | --- | --- |
| Channels per process | 1 | N (single-process multi-channel) |
| Detector | Motion only | Motion or Person (YOLOX) |
| Output binary | `SampleDLL.dll` (non-`GAI_*` exports) | `GenericAI.Native.dll` (`GAI_*` exports) |
| Recorder cmdline change | none | `channel_count=N` added |

The recorder side decides which exe to spawn via `AStreamPerimeter_GenericAI`. Both binaries can coexist on disk; they share no DLLs and don't conflict.

## Project Layout

```
GenericAI/
  GenericAI.sln
  GenericAI.App/
    Program.cs              entry point + cleanup orchestration
    CommandLineArgs.cs      cmdline parse
    ChannelHandle.cs        per-channel state (listener / MMF reader / param store)
    NativeInterop.cs        P/Invoke surface for GAI_* exports
    FrameDispatcher.cs      detector callback → channel router
    EncodeWorker.cs         JPEG encode worker (turbojpeg)
    SendWorker.cs           HTTP POST worker
    HttpListenerHost.cs     /Alive, /GetLicense, /SetParameters
    ParameterStore.cs       most recent SetParameters per channel
    FileLogger.cs           file logger (D:\SLog-<basePort>)
    TimingRecorder.cs       optional timing instrumentation
  GenericAI.Native/
    exports.cpp             GAI_* C ABI
    shared_detector_scheduler.{h,cpp}   single-detector + N-channel scheduling
    channel_pipeline.{h,cpp}            per-channel work queue
    detector_factory.{h,cpp}            picks Motion or Person at compile time
    detector_motion.{h,cpp}             frame-diff detector
    detector_person.{h,cpp}             YOLOX-M FP16 detector (DirectML / CPU)
    mmf_reader.{h,cpp}                  per-channel MMF poll loop
    gai_abi.h / gai_config.h            ABI declarations + compile-time flags
  native-deps/
    models/yolox_m_fp16.onnx
    win-x64/turbojpeg.dll + LICENSE.md + VERSION.txt
```
