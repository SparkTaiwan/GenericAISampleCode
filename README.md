# GenericAISampleCode

Sample and reference code for integrating an external AI/analytics module with **Spark Recorder** (and compatible NVR/CMS systems). The recorder streams encoded video to an AI wrapper process, the wrapper runs detection and streams analytics results back. Control and configuration use **HTTP/JSON**; video frames and results use a **ZMQ** transport (with a legacy shared-memory + HTTP path kept for backward compatibility).

## Project structure

The repository currently hosts three variants:

| Folder | What it is | Detectors | Transport | Own README |
| --- | --- | --- | --- | --- |
| [`GenericAI_ZMQ/`](GenericAI_ZMQ/) | **Production** multi-channel wrapper. One process serves N channels through a shared detector backend, with full diagnostics (file logging, timing, health/degraded reporting). | Motion (frame-diff) **and** YOLOX-M person (ONNX Runtime / DirectML, CPU fallback) | ZMQ (default) **or** MMF+HTTP (legacy) | [EN](GenericAI_ZMQ/README.md) / [繁中](GenericAI_ZMQ/README_zhTW.md) |
| [`GenericAI_Sample_ZMQ/`](GenericAI_Sample_ZMQ/) | **Minimal reference** for the packet flow. Same transport + HTTP control + settings-schema contract as the production wrapper, but the detector stack and diagnostic infra are stripped so the wire behaviour is easy to read. | Motion only | ZMQ **or** MMF+HTTP | — |
| [`GenericAI_Linux/`](GenericAI_Linux/) | Placeholder for a Linux port (not yet populated). | — | — | — |

Both wrappers are the same shape: a **C# host** (`GenericAI.exe`, .NET Framework 4.8) plus a **C++ detector DLL** (`GenericAI.Native.dll`) exporting a `GAI_*` C ABI. The C# side owns the HTTP control listeners, the ZMQ result sender / MMF readers, and JPEG encode + send; the native side owns frame decode (H.264/H.265 NAL over ZMQ) and detection.

### Which one should I use?

- **Integrating / deploying** → `GenericAI_ZMQ/`. It is the wrapper Spark Recorder's `AStreamPerimeter_GenericAI` AI-service module talks to in production.
- **Learning the protocol / building your own wrapper** → `GenericAI_Sample_ZMQ/`. Start here to see the minimal control + frame + result flow without the detector and diagnostics noise.

## Architecture and data flow

Three independent planes connect the recorder (left) and the AI wrapper (right):

```text
Spark Recorder / AStreamPerimeter_GenericAI        GenericAI.exe (wrapper)
+-------------------------------+                  +-------------------------------+
| HTTP client  (control plane)  | --(1) request--> | HTTP listeners (per channel)  |
|  - GET  /Alive                |                  |  - /Alive  (health / version) |
|  - GET  /GetSettingsSchema    | <--(1a) reply--- |  - /GetSettingsSchema (UI)     |
|  - POST /SetParameters        |                  |  - /SetParameters (config)    |
+-------------------------------+                  +-------------------------------+

+-------------------------------+                  +-------------------------------+
| Frame plane (source)          | ==(2) frames===> | Frame plane (sink)            |
|  ZMQ PUSH  (encoded H.264/5)  |                  |  ZMQ PULL -> NAL decode       |
|   -- or legacy --             |                  |   -- or legacy --             |
|  MMF writer (raw I420)        |                  |  MMF reader                   |
+-------------------------------+                  +-------------------------------+

+-------------------------------+                  +-------------------------------+
| Result plane (sink)           | <=(3) results=== | Result plane (source)         |
|  ZMQ PULL (analytics JSON)    |                  |  ZMQ PUSH                     |
|   -- or legacy --             |                  |   -- or legacy --             |
|  HTTP /PostAnalyticsResult    |                  |  HTTP POST                    |
+-------------------------------+                  +-------------------------------+
```

1. **Control plane (HTTP/JSON).** The recorder probes `/Alive`, fetches the dynamic settings UI with `/GetSettingsSchema`, and pushes per-channel configuration (resolution, ROIs, AI settings) with `POST /SetParameters`. These listeners stay on the wrapper's control port (`port=` = the recorder's `exe_server_port`) regardless of the frame/result transport.
2. **Frame plane.** In ZMQ mode the recorder PUSHes encoded access units and the wrapper decodes them (offloading decode to the AI host, so it works remotely). In legacy MMF mode the recorder writes raw I420 frames into per-channel shared memory.
3. **Result plane.** Analytics results (JSON: `port_num`, keyframe JPEG, ROI hits, per-class counts) go back over a ZMQ PUSH→PULL pair, or, in legacy mode, an HTTP POST to `analytics_event_api_url` (`/PostAnalyticsResult`).

The recorder binds the ZMQ sockets and the wrapper connects to them, so the wrapper can be (re)started independently and ZMQ reconnects automatically. Frame and result planes use **separate** ports: the frame plane connects to the recorder's `ai_stream_port`, the result plane to its `http_server_port`. See [GenericAI_ZMQ/README.md](GenericAI_ZMQ/README.md) for the full launch reference.

## Settings schema and scopes

`GET /GetSettingsSchema` returns a JSON schema the ConfigClient renders into the AI-settings UI; the filled values come back verbatim in `SetParameters`. Every field declares a **scope** that says where it is edited and how it flows on the wire:

| scope | one value per… | edited on | carried in |
| --- | --- | --- | --- |
| `device` | whole device | device page | top-level `ai_settings` (device-wide) |
| `channel` | smart stream | stream page | top-level `SetParameters.ai_settings` |
| `roi` | detection region | ROI editor | per entry in `SetParameters.rois[i]` |

Rule of thumb: "what counts as a hit" (detection tuning) → `roi`; "how we output / the whole stream" → `channel`; "one setting for the entire device" → `device`. `GenericAI_Sample_ZMQ` ships a motion schema with one field of every scope as a worked example (see its `SettingsSchema.cs`).

## Getting started

Prerequisites and full build/run instructions live in the per-project README. In short, from a wrapper folder:

```powershell
nuget restore GenericAI.sln
msbuild GenericAI.sln /p:Configuration=Release /p:Platform=x64
```

Output lands in `bin/Release/x64/`. Requirements (VS 2019/2022 with the **MSVC v140** toolset, .NET Framework 4.8, optional DirectML GPU) and the full dependency list are in [GenericAI_ZMQ/README.md](GenericAI_ZMQ/README.md).

## Launching: ZMQ mode vs MMF mode

The wrapper picks its transport **purely from the launch arguments** — specifically, which ZMQ endpoints you pass. Nothing else selects the mode:

- Pass a ZMQ endpoint for a plane → that plane uses ZMQ.
- Omit it → that plane falls back to the legacy MMF/HTTP path.

The frame plane and the result plane are chosen independently, so a mixed setup is possible, but the two normal configurations are below. In both, `port=` is always the **HTTP control port** (`/Alive`, `/SetParameters`) and equals the recorder's `exe_server_port`; only the frame/result **data** planes move.

### ZMQ mode (production — works local or remote)

The recorder BINDs the ZMQ sockets and the wrapper CONNECTs to them. Pass **both** endpoints, or the `server_ip` shorthand:

```text
:: explicit endpoints
GenericAI.exe port=<exe_server_port> channel_count=<N> mode=single detector=motion ^
              frame_endpoint=tcp://<recorderIP>:<ai_stream_port> ^
              result_endpoint=tcp://<recorderIP>:<http_server_port>

:: shorthand — expands to the two endpoints above
GenericAI.exe port=<exe_server_port> channel_count=<N> mode=single detector=motion ^
              server_ip=<recorderIP> stream_port=<ai_stream_port> result_port=<http_server_port>
```

- **Frames** arrive over ZMQ as encoded H.264/H.265 (the wrapper decodes them) → the frame plane connects to the recorder's **`ai_stream_port`**.
- **Results** are PUSHed back over ZMQ → the result plane connects to the recorder's **`http_server_port`** (a ZMQ PULL socket bound there — the port number is reused from that config field; it is **not** an HTTP server in ZMQ mode).
- The three ports are independent: `port=` (control) ≠ `stream_port` (frames) ≠ `result_port` (results). A common mistake is pointing `result_port` at the stream port — then frames flow but results never arrive.
- Requires **`mode=single`** (one process serves every channel, multiplexed by `channel_id`). ZMQ reconnects automatically, so the wrapper and recorder can start in any order.

### MMF mode (legacy — same machine only)

Pass **no** ZMQ endpoints. Frames come from shared memory and results go back over HTTP:

```text
:: one process, all channels
GenericAI.exe port=<exe_server_port> channel_count=<N> mode=single detector=motion

:: one process per channel — launched once per channel port
GenericAI.exe port=<channelPort> mode=multi detector=motion
```

- **Frames** are read from a per-channel named shared-memory segment (`ChannelFrame_<port>`) that the recorder writes raw I420 into.
- **Results** are HTTP-POSTed to the `analytics_event_api_url` the recorder supplies in `SetParameters` (its `/PostAnalyticsResult` endpoint).
- Shared memory is local, so both processes must be on the **same machine**. The ffmpeg/libzmq DLLs are not exercised on this path.

### Quick reference

| | ZMQ mode | MMF mode |
| --- | --- | --- |
| Frame source | `frame_endpoint` set → ZMQ PULL (`ai_stream_port`) | unset → shared memory `ChannelFrame_<port>` |
| Result sink | `result_endpoint` set → ZMQ PUSH (`http_server_port`) | unset → HTTP POST `/PostAnalyticsResult` |
| Location | local **or** remote | local only |
| `mode` | must be `single` | `single` or `multi` |
| Codec on the wire | encoded H.264/H.265 (decoded by wrapper) | raw I420 |

Running with **no** arguments (or F5 in Visual Studio) defaults to `port=46000`, one channel, MMF+HTTP, and the compiled default detector — a handy smoke test. Turn on `show_debug` (below) and the startup banner prints the chosen transport for each plane. Full per-argument reference: [GenericAI_ZMQ/README.md](GenericAI_ZMQ/README.md).

## Runtime debug switches (`GenericAI_ZMQ` only)

The production wrapper reads a plain-text `GenericAI.Config` next to `GenericAI.exe` — no rebuild needed, restart to apply. Values accept `1 / true / yes / on`:

| Key | Effect |
| --- | --- |
| `show_debug` | C# verbose console output (SetParameters echo, `Detected!!`, and the `[ZMQ result] zmq_send OK/FAILED …` line at the send point). |
| `show_native_debug` | Native informational console logs (`[MotionDetector]`, `[PersonDetector]`, `[AI]`, `[channel]`, `[zmq]`). |
| `log_to_file` | Persist INFO/WARN/ERROR to `%ProgramData%\Spark\GenericAI\Logs\GenericAI-<basePort>.log`. |

`GenericAI_Sample_ZMQ` keeps things minimal and logs directly to the console (no config gate).

## License

Apache License 2.0.
