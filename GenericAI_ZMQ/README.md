# GenericAI (ZMQ)

Multi-channel AI wrapper that companions Spark Recorder's `AStreamPerimeter_GenericAI` AI-service module. One process serves N independent video channels through a single shared detector backend.

Two transports are supported and chosen at launch (see **Run**):

- **ZMQ (default in production)** — the recorder PUSHes encoded H.264/H.265 access units over a ZMQ frame plane; the wrapper decodes them and PUSHes analytics results back over a ZMQ result plane. Works local **or** remote.
- **MMF + HTTP (legacy / local-only)** — the recorder writes raw I420 frames into per-channel shared memory and the wrapper HTTP-POSTs results back. Kept for backward compatibility; used only when no ZMQ endpoints are passed.

Traditional Chinese: see [README_zhTW.md](README_zhTW.md).

## Components

| Project              | Type                    | Output                   | Purpose                                                                                                          |
| -------------------- | ----------------------- | ------------------------ | -------------------------------------------------------------------------------------------------------------- |
| `GenericAI.App`    | C# (.NET Framework 4.8) | `GenericAI.exe`        | Host process: HTTP control listeners, ZMQ result sender / MMF readers, JPEG encode + send worker pools, native interop. |
| `GenericAI.Native` | C++ (MSVC v140)         | `GenericAI.Native.dll` | Detector implementations, per-channel pipeline, ZMQ frame receiver + NAL (H.264/H.265) decode. Exports the `GAI_*` C ABI. |

## Requirements

- Windows 10 / 11, x64.
- Visual Studio 2019 or 2022 with the **MSVC v140 toolset** and **Windows 8.1 SDK** (the C++ project targets v140 to match Spark Recorder's link environment).
- .NET Framework **4.8** Developer Pack.
- DirectML-capable GPU recommended for the Person detector; the loader falls back to CPU automatically.

Third-party dependencies (restored via NuGet):

| Package | Version | Used by | For |
| --- | --- | --- | --- |
| `Newtonsoft.Json` | 13.0.3 | App | `/SetParameters` parse + result JSON. |
| `System.Buffers` | 4.5.1 | App | Pooled buffers on the frame path. |
| `Microsoft.ML.OnnxRuntime.DirectML` | 1.14.1 | Native | Person detector inference. |
| `Microsoft.AI.DirectML` | 1.10.1 | Native | DirectML execution provider. |
| `libzmq-vc143` | 4.2.0 | Native | ZMQ frame + result transport. |
| `FFmpeg-lgpl3` | 4.4.1 | Native | H.264 / H.265 NAL decode (ZMQ frame plane). |
| `libjpeg-turbo` (bundled at `native-deps/win-x64/turbojpeg.dll`) | — | App | Callback JPEG encoding. |

The Person detector model is bundled at `native-deps/models/yolox_m_fp16.onnx` (YOLOX-M, FP16, COCO-trained — class 0 = person). The path is resolved relative to the exe.

## Build

```powershell
# from the GenericAI_ZMQ folder (the solution root)
nuget restore GenericAI.sln
msbuild GenericAI.sln /p:Configuration=Release /p:Platform=x64
```

Or open `GenericAI.sln` in Visual Studio, pick `x64 / Release`, and build. Output lands in `bin/Release/x64/`.

## Release contents

Everything a deployment needs is in `bin/Release/x64/` after a Release build:

| File | Purpose |
| --- | --- |
| `GenericAI.exe` + `GenericAI.exe.config` | Host process. |
| `GenericAI.Native.dll` | Detector pipeline + ZMQ receiver + NAL decode (`GAI_*` C ABI). |
| `onnxruntime.dll`, `DirectML.dll` | ONNX Runtime + DirectML EP for the Person detector. |
| `turbojpeg.dll` + `turbojpeg.LICENSE.md` | Callback JPEG encoding (license must ship with the dll). |
| `libzmq-v143-mt-4_2_0.dll`, `libsodium.dll` | ZMQ transport (frame + result planes). |
| `avcodec-58.dll`, `avformat-58.dll`, `avutil-56.dll`, `swscale-5.dll`, `swresample-3.dll`, `avfilter-7.dll`, `avdevice-58.dll`, `postproc-55.dll` | FFmpeg — H.264/H.265 decode of ZMQ frames. |
| `Newtonsoft.Json.dll`, `System.Buffers.dll` | Managed dependencies. |
| `models\` (`yolox_m_fp16.onnx` + `LICENSE.md`) | Person detector model (Apache 2.0 text must ship with it). |

`*.pdb` / `*.lib` / `*.exp` / `*.iobj` / `*.ipdb` are build artifacts and need not ship.

## Run

### 1. Command-line arguments

```
GenericAI.exe port=<int> [channel_count=<N>] [mode=single|multi] [detector=motion|objectdetection]
              [server_ip=<recorderIP> result_port=<port> stream_port=<port>]
              [frame_endpoint=tcp://host:port] [result_endpoint=tcp://host:port]
              [http_host=<ip|+>] [encode_workers=<N>] [send_workers=<M>]
```

Arguments are `key=value` pairs in any order. An unknown key, a missing `=`, or an out-of-range value exits with code `2`.

| Argument           | Default       | Meaning                                                                                                        |
| ------------------ | ------------- | ------------------------------------------------------------------------------------------------------------- |
| `port`           | `46000`     | Base HTTP **control** port (`/Alive`, `/SetParameters`, `/GetLicense`, `/GetSettingsSchema`). Channel `k` (0-indexed) listens on `port + k`. |
| `channel_count`  | `1`         | Number of channels served by this process. No upper bound; `<1` is rejected.                                |
| `mode`           | `multi`     | `single` = this process serves `channel_count` channels; `multi` = one process per channel. Informational (topology is driven by `channel_count`); accepted so the recorder's `mode=` is not rejected. |
| `detector`       | *(compile-time default: Motion)* | `motion` or `objectdetection` (aliases: `person`, `objdetection`). Overrides the `gai_config.h` default at runtime. Omit to use the compiled default. |
| `server_ip` + `result_port` + `stream_port` | *(unset)* | Shorthand that expands to `result_endpoint=tcp://server_ip:result_port` and `frame_endpoint=tcp://server_ip:stream_port`. **Must be given together.** Use the recorder's `http_server_port` for `result_port` and its `ai_stream_port` for `stream_port` (the two are independent). |
| `frame_endpoint` | *(unset)*   | Explicit ZMQ frame-plane address the wrapper PULLs from (e.g. `tcp://127.0.0.1:5556`). Takes precedence over the shorthand. When unset → MMF frames. |
| `result_endpoint`| *(unset)*   | Explicit ZMQ result-plane address the wrapper PUSHes to. Takes precedence over the shorthand. When unset → HTTP POST results. |
| `http_host`      | `127.0.0.1` | Bind address for the HTTP control ports. Loopback = local-only. For a **remote** wrapper pass the machine's reachable IP (`http_host=172.20.1.18`) or `+`/`*` for all interfaces. A non-loopback host needs admin or a URL reservation (`netsh http add urlacl`). |
| `encode_workers` | `2`         | JPEG-encode worker threads (process-wide pool, 1..16).                                                        |
| `send_workers`   | `2`         | Result send worker threads (process-wide pool, 1..16).                                                        |

**Transport is selected by which endpoints are present:**

- Both `frame_endpoint` and `result_endpoint` set → full ZMQ (frames in over ZMQ, results out over ZMQ).
- Neither set → legacy MMF frames + HTTP result POST.
- Each plane is independent, so a mixed setup (e.g. ZMQ frames + HTTP results) is possible if you set only one.

### 2. Standalone / debug run

Running with no args (or via F5 in Visual Studio) uses `port=46000`, one channel, MMF+HTTP, and the compiled default detector — handy for a quick smoke test:

```powershell
.\GenericAI.exe
```

Force a specific detector and 4 channels over MMF+HTTP:

```powershell
.\GenericAI.exe port=46000 channel_count=4 detector=objectdetection
```

Run against a recorder that BINDs its ZMQ sockets on `172.20.1.36` (result plane on the recorder's `http_server_port`, frame plane on its `ai_stream_port`), using the shorthand:

```powershell
.\GenericAI.exe port=46000 mode=single channel_count=4 detector=objectdetection `
    server_ip=172.20.1.36 result_port=9905 stream_port=9906
```

### 3. How the recorder spawns it (production)

The recorder decides the exe and arguments in `AStreamPerimeter_GenericAI` / `GenericAIDevice`. You normally don't launch it by hand — this is documented so you can reproduce a spawn for debugging.

**Single mode with ZMQ (built-in detector, `getUseZmq()==true`)** — the recorder BINDs both ZMQ sockets and the wrapper dials `127.0.0.1`:

```
GenericAI.exe port=<exe_server_port> mode=single channel_count=<N> detector=motion|objectdetection ^
              frame_endpoint=tcp://127.0.0.1:<ai_stream_port> ^
              result_endpoint=tcp://127.0.0.1:<http_server_port>
```

**Multi mode (one process per channel, MMF + HTTP)** — external AIs only understand `port=`; the built-in also gets `mode`/`detector`:

```
GenericAI.exe port=<channelPort> mode=multi detector=motion|objectdetection
```

Notes:
- The HTTP control port stays on `port=` (== `exe_server_port`, channel 0) in every mode; only the frame/result **data** planes move to ZMQ.
- A **remote** wrapper is not spawned by the recorder — an operator runs it by hand on the other host with `http_host=<that host's IP>` and the `server_ip`/endpoint args pointing back at the recorder.
- ZMQ requires `mode=single`; the recorder logs a warning and does not enable ZMQ for `mode=multi`.

### 4. Exit codes

| Code  | Meaning                                            |
| ----- | -------------------------------------------------- |
| `0` | OK                                                 |
| `1` | Generic failure                                    |
| `2` | Bad arguments                                      |
| `3` | Port already in use                                |
| `4` | Native init failed (detector / ONNX session / ZMQ / MMF) |

A **degraded** init (detector/model failed to load, native returns `5`) is *not* an exit: the process stays up, the HTTP listeners keep serving, and `/Alive` reports `{"status":"error","version":"...","message":"..."}` so the recorder surfaces the reason instead of restart-looping the process.

## Logging & Diagnostics

All switches are compile-time constants: edit one line, rebuild, restart the exe.

| Switch                   | Location                                    | Default   | Effect                                                                                                          |
| ------------------------ | ------------------------------------------- | --------- | --------------------------------------------------------------------------------------------------------------- |
| `FileLogger.Enabled`   | `GenericAI.App/Diagnostics/FileLogger.cs` | `true`  | INFO / WARN / ERROR file logging, including the native-side lines forwarded through the log callback.            |
| `ConsoleLog.Enabled`   | `GenericAI.App/Diagnostics/ConsoleLog.cs` | `true` | Console status lines (HTTP state, SetParameters echo, send results).                                             |
| `TimingRecorder.Enabled` | `GenericAI.App/Diagnostics/TimingRecorder.cs` | `false` | C#-side per-frame timing lines (console + `timing-<basePort>.log`) and the GenericAI-specific verbose console lines. |
| `kEnableTimingLog`     | `GenericAI.Native/gai_config.h`           | `false` | Native-side per-frame timing lines and the native informational console lines.                                  |

Log files land in `%ProgramData%\Spark\GenericAI\Logs\`:

- `GenericAI-<basePort>.log` — INFO / WARN / ERROR
- `error-<basePort>.log` — ERROR duplicated for quick triage
- `timing-<basePort>.log` — per-frame timing lines from the C# `TimingRecorder`

Writing is asynchronous: producers enqueue into a bounded queue and a single background thread drains it, so logging never blocks the frame path. Files rotate at 5 MB with 3 backups, and names carry the base port so concurrent instances never contend for the same file.

With file logging enabled, native-side diagnostics (pool sizing, MMF mapping, decode/ZMQ errors, dropped-frame warnings) reach the same per-port log file with a `[native]` prefix: the host registers a sink via `GAI_RegisterLogCallback` at startup, so one file tells the whole story across the managed/native boundary.

## Wire Protocol

The HTTP control plane, MMF layout, and ZMQ frame/result planes match `spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp` + `GenericAIDevice.cpp` + `ZmqFrameProtocol.h` so the recorder side stays unchanged.

### HTTP control plane (recorder ↔ wrapper)

Each channel's control endpoints live on its own port (`port + k`). This plane is HTTP in **every** transport mode — only the frame/result data planes move to ZMQ.

| Endpoint                   | Direction           | Purpose                                                                                                                                                                                                 |
| -------------------------- | ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `GET /Alive`             | recorder → wrapper | Health probe. Returns 200 with `{"status":"ok","version":"1.3"}`, or `{"status":"error","version":"1.3","message":"..."}` when degraded. The recorder reads `version` to pick the protocol level for `/SetParameters` (defaults to `1.2` if absent — old wrappers). |
| `GET /GetLicense`        | recorder → wrapper | License token endpoint (empty body in the built-in).                                                                                                                                                   |
| `GET /GetSettingsSchema` | recorder → wrapper | Self-describing AI-settings schema (spec §5) for the running detector (motion vs object detection). The ConfigClient GETs this to render the dynamic settings UI; filled values return in `SetParameters.ai_settings`. |
| `POST /SetParameters`    | recorder → wrapper | Per-channel config. JSON: `{version, mode, channel_id, analytics_event_api_url, image_width, image_height, jpg_compress, ai_settings, rois: [{sensitivity, threshold, rects: [{x,y}, ...]}]}`. v1.3 sources `jpg_compress` from `ai_settings`; v1.2 keeps the legacy top-level field. |
| `POST <analytics_event_api_url>` | wrapper → recorder | Detection callback (**HTTP-result mode only**), body = JPEG of the keyframe + ROI metadata + `items[]` counts. In ZMQ-result mode the same JSON goes over the ZMQ result plane instead. |

### ZMQ frame plane (recorder → wrapper)

Two-part ZMQ message; recorder is `PUSH(bind)`, wrapper is `PULL(connect)`:

- **part 0** — `ZmqFrameHeader`, packed **28 bytes**: `magic(u32) | version(u16) | codec(u8: 1=H264, 2=H265) | is_keyframe(u8) | width(u16) | height(u16) | channel_id(u32) | timestamp(u64, Windows FILETIME 100ns UTC) | payload_sz(u32)`.
- **part 1** — raw encoded NAL bytes for one coded picture (access unit).

`channel_id` (== `exe_server_port + index`) multiplexes all channels over the single socket pair; the wrapper routes each frame to the `ChannelPipeline` whose `Port() == channel_id` and decodes the NAL to I420. Magic `0x5A4D4600`, version `1`.

### ZMQ result plane (wrapper → recorder)

Single-part ZMQ message carrying the same analytics-result JSON the HTTP callback uses; wrapper is `PUSH(connect)`, recorder is `PULL(bind)`. Parsed by `GenericAIDevice::parseGenericAIHttpAnalyticsEvent()`.

### MMF frame plane (recorder → wrapper, legacy)

- Per-channel name: `ChannelFrame_<channelPort>` (channel `k` reads from `ChannelFrame_{port + k}`).
- Layout mirrors `MMF_Data` in the legacy `CSharp/SampleDLL/dllmain.cpp` to keep the recorder writer unchanged.
- `status` byte: `0=unused → 1=new frame → 2=consumed`. The wrapper polls and flips `1 → 2` after acquiring a frame.
- `image_size` may over-report the packed I420 payload (the recorder sizes its decode buffer with 32-byte-aligned plane strides, e.g. 352x240 reports 130560 bytes for a 126720-byte image). The wrapper validates and copies `width*height*3/2` computed from the frame's own dimensions and tolerates the slack.

## Detector Backends

The default detector is chosen at compile time in `gai_config.h` and can be overridden at runtime with `detector=`:

```cpp
constexpr DetectorKind kDetectorKind = DetectorKind::Motion;   // compile-time default; detector= overrides
constexpr bool         kPreferGpu    = true;                   // Person only
```

A single detector instance is shared across all channels via `SharedDetectorScheduler`; channels round-robin through it and results dispatch back via `FrameDispatcher`. When `kEnablePipelinedInference` is on (default) and the detector supports it (Person does, Motion doesn't), inference splits into a CPU-preprocess loop and a GPU+post loop with a small queue between, overlapping CPU and GPU work across frames.

### Person / object detection — YOLOX-M FP16 ONNX

- DirectML EP with CPU fallback. The actual EP is readable via `GAI_GetBackend`; with the timing/verbose switch on it is also printed at startup (`EP=DirectML(0)` or `EP=CPU`).
- Letterbox preprocess → ONNX inference → score filter + NMS → ROI overlap filter.
- Per-channel ROI list comes from `POST /SetParameters`; class set / confidence / object-size band come from `ai_settings`.

**Tuning (per-call, from the `/SetParameters` payload):**

| Field           | Range  | Effect                                                                                                                                                                                                   |
| --------------- | ------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `threshold`   | 0..100 | Linear-mapped to YOLOX confidence threshold `0.20 .. 0.70`. Higher = stricter (fewer false positives, more misses).                                                                                    |
| `sensitivity` | —     | Unused. YOLOX inference exposes only one effective knob (confidence score), so two sliders would alias to the same axis. The field is still required in the JSON for protocol compatibility with Motion. |

Object-detection `ai_settings` scalars (via `GAI_SetChannelAiSettings`): `confidence` (0..1), `classMask` (bitmask over the fixed class table `person, car, bus, truck, motorcycle, bicycle, cat, dog`), and `min/maxObjectSize` (box-area band as % of frame).

Detector-internal constants (not in the wire protocol): NMS IoU threshold fixed at 0.45; frame decimation `stride_n=1` (every frame runs inference).

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

`POST /SetParameters` ships per-ROI `sensitivity[]` / `threshold[]` arrays, but the tuning pair is semantically **one shared set for the whole channel**: the native side picks the first valid-polygon ROI slot (`rects.size() >= 3`) and uses its `(threshold, sensitivity)` for the whole frame's inference. If no ROI is valid the last good value is retained; before any `/SetParameters` arrives the built-in schema defaults apply (Program.cs seeds every channel with the schema defaults at startup).

### ROI coordinate space

ROI coordinates in `/SetParameters` are authored in the `image_width × image_height` reference space of that payload. Frames (MMF or decoded from ZMQ NAL) carry their own resolution, which may differ (e.g. a sub-stream of the same camera). Before each inference the scheduler rescales the ROI rectangles — and the polygon points the Motion callback echoes — onto the actual frame resolution, so the same ROI keeps covering the same scene area regardless of the stream resolution. Callback coordinates are therefore always in the **actual frame's pixel space**, matching the keyframe JPEG they accompany.

The per-channel frame pool is still sized from the first `/SetParameters`' `image_width`/`image_height` and stays locked for the channel's lifetime: frames **larger** than that are dropped before detection (a throttled console warning reports it), and a later `/SetParameters` with a different resolution is ignored with a warning. Restart the channel to move to a larger resolution.

## Native ABI

`GenericAI.Native.dll` exports a small C ABI consumed by `GenericAI.App` via P/Invoke (`Interop/NativeInterop.cs`):

| Symbol                                                        | Purpose                                                          |
| ------------------------------------------------------------- | ---------------------------------------------------------------- |
| `GAI_InitializeChannels(ports, count, detectorKind)`        | Allocate per-channel pipelines, build detector (`0`=Motion, `1`=Person, `-1`=compile-time default), start scheduler. Returns `5` on a degraded (model-load) init. |
| `GAI_SetChannelParameters(port, *SettingParameters)`        | Push the latest `/SetParameters` payload (url, resolution, ROIs, tuning) for one channel. |
| `GAI_SetChannelAiSettings(port, confidence, classMask, sensitivity, threshold, minObjectSize, maxObjectSize)` | Push the schema `ai_settings` scalars; pass `<0` for keys a detector doesn't use. |
| `GAI_RegisterCallback(cb)`                                  | Register the detection callback fired from `FrameDispatcher`.  |
| `GAI_RegisterLogCallback(cb)`                               | Register the host log sink; native INFO / WARN / ERROR lines land in the per-port file log with a `[native]` prefix. |
| `GAI_GetBackend(buf, len)`                                  | Read back the actual loaded EP (`CPU` / `DirectML(0)`).      |
| `GAI_GetInitError(buf, len)`                                | Read the degraded-init reason (model load failure) to serve on `/Alive`. Returns `0` when healthy. |
| `GAI_StartZmqReceiver(endpoint)`                            | Connect the PULL frame socket to the recorder's bound PUSH; frames then arrive over ZMQ (NAL → decode) instead of MMF. Call after init, before the first `/SetParameters`. |
| `GAI_StopZmqReceiver()`                                     | Stop the ZMQ frame receiver.                                     |
| `GAI_Deinitialize()`                                        | Stop scheduler, free detector, drain queues.                     |

## Relationship to `CSharp/`

`CSharp/` is the legacy single-channel sample wrapper (`SampleWrapper.exe` + `SampleDLL.dll`) preserved as reference code. GenericAI replaces it for production use:

|                         | `CSharp/SampleWrapper.exe`              | `GenericAI.exe`                            |
| ----------------------- | ----------------------------------------- | -------------------------------------------- |
| Channels per process    | 1                                         | N (single-process multi-channel)             |
| Detector                | Motion only                               | Motion or Person (YOLOX)                     |
| Transport               | MMF + HTTP                                 | ZMQ (frame + result) or MMF + HTTP           |
| Output binary           | `SampleDLL.dll` (non-`GAI_*` exports) | `GenericAI.Native.dll` (`GAI_*` exports) |

The recorder side decides which exe to spawn via `AStreamPerimeter_GenericAI`. Both binaries can coexist on disk; they share no DLLs and don't conflict.

## Project Layout

```
GenericAI_ZMQ/
  GenericAI.sln
  GenericAI.App/
    Program.cs              entry point + cleanup orchestration
    CommandLineArgs.cs      cmdline parse (port/channel_count/mode/detector/endpoints/...)
    ChannelHandle.cs        per-channel state (listener + MMF reader + queues)
    Protocol.cs             single source of truth for the wire protocol version
    SettingsSchema.cs       built-in /GetSettingsSchema payload (motion vs objectdetection)
    Http/
      HttpListenerHost.cs   /Alive, /GetLicense, /GetSettingsSchema, /SetParameters
      HttpPostClient.cs     shared HttpClient for HTTP-mode detection callbacks
      HttpEnvelope.cs       result callback JSON payload (version + keyframe + items + rois)
      ParameterStore.cs     process-wide url + jpgQuality cache from /SetParameters
    Pipeline/
      FrameDispatcher.cs    detector callback -> channel router
      EncodeWorker.cs       JPEG encode worker (turbojpeg)
      SendWorker.cs         result send worker (HTTP POST or ZMQ PUSH)
      ZmqResultSender.cs    ZMQ PUSH result sink (connect to the module's bound PULL)
      RoundRobinTaker.cs    fair multi-queue taker for the worker pools
      DropCounter.cs        per-stage drop counters
    Interop/
      NativeInterop.cs      P/Invoke surface for GAI_* exports
      TurboJpegInterop.cs   P/Invoke surface for turbojpeg
      DetectorType.cs       managed mirror of the native DetectorKind
    Diagnostics/
      FileLogger.cs         async file logger (%ProgramData%\Spark\GenericAI\Logs)
      ConsoleLog.cs         gate for the console status lines
      HealthState.cs        process-wide health surfaced on /Alive
      TimingRecorder.cs     optional timing instrumentation
  GenericAI.Native/         flat on disk; grouped in VS via .vcxproj.filters
    exports.cpp             GAI_* C ABI
    host_log.{h,cpp}        native -> host log bridge (GAI_RegisterLogCallback)
    shared_detector_scheduler.{h,cpp}   shared-detector + N-channel scheduling
    channel_pipeline.{h,cpp}            per-channel work queue
    param_snapshot.{h,cpp}              per-channel /SetParameters store (ROIs + tuning params)
    detector_factory.{h,cpp}            picks Motion or Person (compile default + detectorKind)
    detector_motion.{h,cpp}             frame-diff detector
    detector_person.{h,cpp}             YOLOX-M FP16 detector (DirectML / CPU)
    mmf_reader.{h,cpp}                  per-channel MMF poll loop
    zmq_frame_receiver.{h,cpp}          ZMQ PULL frame receiver
    zmq_frame_header.h                  28-byte frame header (mirrors recorder ZmqFrameProtocol.h)
    nal_decoder.{h,cpp}                 H.264/H.265 NAL -> I420 (FFmpeg)
    buffer_pool.{h,cpp}                 pooled frame buffers
    class_table.h                       fixed detectable-class order
    detector geometry: roi_geometry.h, yolox_geometry.h
    gai_abi.h / gai_config.h            ABI declarations + compile-time flags
  native-deps/
    models/yolox_m_fp16.onnx + LICENSE.md + VERSION.txt
    win-x64/turbojpeg.dll + LICENSE.md + VERSION.txt
```
