# GenericAI

Multi-channel AI wrapper that companions Spark Recorder's `AStreamPerimeter_GenericAI` AI-service module. One process serves N independent video channels through a single shared detector backend.

## Components

| Project              | Type                    | Output                   | Purpose                                                                                          |
| -------------------- | ----------------------- | ------------------------ | ------------------------------------------------------------------------------------------------ |
| `GenericAI.App`    | C# (.NET Framework 4.8) | `GenericAI.App.exe`    | Host process: HTTP listeners, MMF readers, JPEG encode + HTTP send worker pools, native interop. |
| `GenericAI.Native` | C++ (MSVC v140)         | `GenericAI.Native.dll` | Detector implementations + per-channel pipeline. Exports the `GAI_*` C ABI.                    |

## Requirements

- Windows 10 / 11, x64.
- Visual Studio 2019 or 2022 with the **MSVC v140 toolset** and **Windows 8.1 SDK** (the C++ project targets v140 to match Spark Recorder's link environment).
- .NET Framework **4.8** Developer Pack.
- DirectML-capable GPU recommended for the Person detector; the loader falls back to CPU automatically.

Third-party dependencies (restored via NuGet):

- `Microsoft.ML.OnnxRuntime.DirectML` 1.14.1 — Person detector inference.
- `Microsoft.AI.DirectML` 1.10.1.
- `Newtonsoft.Json` 13.0.3 — HTTP `POST /SetParameters` payload.
- `libjpeg-turbo` (bundled at `native-deps/win-x64/turbojpeg.dll`) — callback JPEG encoding.

The Person detector model is bundled at `native-deps/models/yolox_m_fp16.onnx` (YOLOX-M, FP16, COCO-trained — class 0 = person). The path is resolved relative to the exe.

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

| Argument           | Default       | Meaning                                                                           |
| ------------------ | ------------- | --------------------------------------------------------------------------------- |
| `port`           | `51000`     | Base sample / callback port. Channel `k` (0-indexed) listens on `port + 2*k`. |
| `channel_count`  | `1`         | Number of channels served by this process. No upper bound;`<1` is rejected.     |
| `encode_workers` | `2`         | JPEG-encode worker threads (process-wide pool, shared across channels).           |
| `send_workers`   | `2`         | HTTP-POST worker threads (process-wide pool).                                     |
| `log`            | `""` (auto) | Override the log directory. Defaults to `D:\SLog-<basePort>\GenericAI.log`.     |

Exit codes:

| Code  | Meaning                                            |
| ----- | -------------------------------------------------- |
| `0` | OK                                                 |
| `1` | Generic failure                                    |
| `2` | Bad arguments                                      |
| `3` | Port already in use                                |
| `4` | Native init failed (detector / ONNX session / MMF) |

Production-side spawn always passes `port=` explicitly. Running the exe without args picks `port=51000` ("Debug Run" mode) so you can press F5 from Visual Studio.

## Wire Protocol

The HTTP control plane and MMF layout match `spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp` exactly so the recorder side stays unchanged.

### HTTP (recorder ↔ wrapper)

Each channel listens on its own port:

| Endpoint                           | Direction           | Purpose                                                                                                                                                                                                                                               |
| ---------------------------------- | ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET /Alive`                     | recorder → wrapper | Health probe, returns 200 OK.                                                                                                                                                                                                                         |
| `GET /GetLicense`                | recorder → wrapper | License token endpoint.                                                                                                                                                                                                                               |
| `POST /SetParameters`            | recorder → wrapper | v1.2 JSON:`{analytics_event_api_url, image_width, image_height, jpg_compress, rois: [{sensitivity, threshold, rects: [{x,y}, ...]}]}`. How each detector consumes `sensitivity` / `threshold` differs — see **Detector Backends** below. |
| `POST <analytics_event_api_url>` | wrapper → recorder | Detection callback, body = JPEG of the keyframe + ROI metadata.                                                                                                                                                                                       |

### MMF (recorder → wrapper)

- Per-channel name: `ChannelFrame_<channelPort>` (channel `k` reads from `ChannelFrame_{port + 2*k}`).
- Layout mirrors `MMF_Data` in the legacy `CSharp/SampleDLL/dllmain.cpp` to keep the recorder writer unchanged.
- `status` byte: `0=unused → 1=new frame → 2=consumed`. The wrapper polls and flips `1 → 2` after acquiring a frame.

## Detector Backends

The active detector is selected at compile time via `gai_config.h`:

```cpp
constexpr DetectorKind kDetectorKind = DetectorKind::Person;   // or DetectorKind::Motion
constexpr bool         kPreferGpu    = true;                   // Person only
```

A single detector instance is shared across all channels via `SharedDetectorScheduler`; channels round-robin through it and results dispatch back via `FrameDispatcher`. When `kEnablePipelinedInference` is on (default) and the detector supports it (Person does, Motion doesn't), inference splits into a CPU-preprocess loop and a GPU+post loop with a small queue between, overlapping CPU and GPU work across frames.

### Person — YOLOX-M FP16 ONNX

- DirectML EP with CPU fallback. Actual EP logged at startup (`EP=DirectML(0)` or `EP=CPU`) and readable via `GAI_GetBackend`.
- Letterbox preprocess → ONNX inference → score filter + NMS → ROI overlap filter.
- Per-channel ROI list comes from `POST /SetParameters`.

**Tuning (per-call, from the `/SetParameters` payload):**

| Field           | Range  | Effect                                                                                                                                                                                                   |
| --------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `threshold`   | 0..100 | Linear-mapped to YOLOX confidence threshold `0.20 .. 0.70`. Higher = stricter (fewer false positives, more misses).                                                                                    |
| `sensitivity` | —     | Unused. YOLOX inference exposes only one effective knob (confidence score), so two sliders would alias to the same axis. The field is still required in the JSON for protocol compatibility with Motion. |

Detector-internal constants (not in the wire protocol):

- NMS IoU threshold: fixed at construction (0.45).
- Target class: fixed at construction (0 = person on COCO).
- Frame decimation: `stride_n=1` (every frame runs inference).

### Motion — frame difference

- Per-pixel Y-plane diff against the previous frame, subsampled, fused per-ROI scan with early exit.
- No model file, CPU only.

**Tuning (per-call, from the `/SetParameters` payload):**

| Field           | Range  | Effect                                                                                                                               |
| --------------- | ------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| `threshold`   | 0..100 | Mapped to the per-pixel diff threshold `8 .. 40` (8-bit grayscale). Higher = only larger luminance changes count.                  |
| `sensitivity` | 0..100 | Mapped to the minimum changed-pixel ratio `0.20 .. 0.005` of each ROI's own area. Higher = fewer changed pixels needed to trigger. |

The two axes are orthogonal: `threshold` is "how strong does each pixel change have to be", `sensitivity` is "how many such pixels do we need".

### Parameter propagation

`POST /SetParameters` ships per-ROI `sensitivity[]` / `threshold[]` arrays. The native side picks the first valid-polygon ROI slot (`rects.size() >= 3`) and uses its `(threshold, sensitivity)` for the whole frame's inference. If no ROI is valid the last good value is retained; before any `/SetParameters` arrives the built-in defaults (`threshold=25`, `sensitivity=50`) apply.

## Native ABI

`GenericAI.Native.dll` exports a small C ABI consumed by `GenericAI.App` via P/Invoke (`NativeInterop.cs`):

| Symbol                                            | Purpose                                                          |
| ------------------------------------------------- | ---------------------------------------------------------------- |
| `GAI_InitializeChannels(ports, count)`          | Allocate per-channel pipelines, build detector, start scheduler. |
| `GAI_SetChannelParameters(port, *GAI_Settings)` | Push the latest `/SetParameters` payload for one channel.      |
| `GAI_RegisterCallback(cb)`                      | Register the detection callback fired from `FrameDispatcher`.  |
| `GAI_GetBackend(buf, len)`                      | Read back the actual loaded EP (`CPU` / `DirectML(0)`).      |
| `GAI_Deinitialize()`                            | Stop scheduler, free detector, drain queues.                     |

## Relationship to `CSharp/`

`CSharp/` is the legacy single-channel sample wrapper (`SampleWrapper.exe` + `SampleDLL.dll`) preserved as reference code. GenericAI replaces it for production use:

|                         | `CSharp/SampleWrapper.exe`              | `GenericAI.App.exe`                        |
| ----------------------- | ----------------------------------------- | -------------------------------------------- |
| Channels per process    | 1                                         | N (single-process multi-channel)             |
| Detector                | Motion only                               | Motion or Person (YOLOX)                     |
| Output binary           | `SampleDLL.dll` (non-`GAI_*` exports) | `GenericAI.Native.dll` (`GAI_*` exports) |
| Recorder cmdline change | none                                      | `channel_count=N` added                    |

The recorder side decides which exe to spawn via `AStreamPerimeter_GenericAI`. Both binaries can coexist on disk; they share no DLLs and don't conflict.

## Project Layout

```
GenericAI/
  GenericAI.sln
  GenericAI.App/
    Program.cs              entry point + cleanup orchestration
    CommandLineArgs.cs      cmdline parse
    ChannelHandle.cs        per-channel state (listener + MMF reader)
    NativeInterop.cs        P/Invoke surface for GAI_* exports
    FrameDispatcher.cs      detector callback → channel router
    EncodeWorker.cs         JPEG encode worker (turbojpeg)
    SendWorker.cs           HTTP POST worker
    HttpListenerHost.cs     /Alive, /GetLicense, /SetParameters
    ParameterStore.cs       process-wide url + jpgQuality cache from /SetParameters
    FileLogger.cs           file logger (D:\SLog-<basePort>)
    TimingRecorder.cs       optional timing instrumentation
  GenericAI.Native/
    exports.cpp             GAI_* C ABI
    shared_detector_scheduler.{h,cpp}   shared-detector + N-channel scheduling
    channel_pipeline.{h,cpp}            per-channel work queue
    param_snapshot.{h,cpp}              per-channel /SetParameters store (ROIs + tuning params)
    detector_factory.{h,cpp}            picks Motion or Person at compile time
    detector_motion.{h,cpp}             frame-diff detector
    detector_person.{h,cpp}             YOLOX-M FP16 detector (DirectML / CPU)
    mmf_reader.{h,cpp}                  per-channel MMF poll loop
    gai_abi.h / gai_config.h            ABI declarations + compile-time flags
  native-deps/
    models/yolox_m_fp16.onnx
    win-x64/turbojpeg.dll + LICENSE.md + VERSION.txt
```
