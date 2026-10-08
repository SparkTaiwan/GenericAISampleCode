# GenericAI (Linux)

Linux build of the GenericAI wrapper in [`../GenericAI_ZMQ`](../GenericAI_ZMQ/README.md). Same command line, same HTTP control plane, same ZMQ / MMF wire formats, same detectors — the recorder side does not change.

| | Windows (`GenericAI_ZMQ`) | Linux (this folder) |
| --- | --- | --- |
| Host process | C# .NET Framework 4.8 (`GenericAI.App`) | C++17 rewrite (`host/`), same behaviour |
| Native | `GenericAI.Native.dll` (P/Invoke) | `native/` (ported copy), linked into the executable |
| Output | `GenericAI.exe` + DLLs | `GenericAI` + `libonnxruntime*.so` |
| GPU EP | DirectML → CPU fallback | CUDA → CPU fallback |
| MMF frames | `CreateFileMapping("ChannelFrame_<port>")` | `shm_open("/ChannelFrame_<port>")` (= Linux recorder `mmf.cpp`) |
| HTTP | `HttpListener` (needs urlacl for non-loopback) | cpp-httplib (no reservation needed) |
| Logs | `%ProgramData%\Spark\GenericAI\Logs` | `/var/log/spark/GenericAI` (see [Logging](#logging)) |
| Build | `GenericAI.sln` / MSBuild | CMake |

## Layout

```text
GenericAI_Linux/
  CMakeLists.txt
  GenericAI.Config            runtime switches (copied next to the executable)
  host/                       C++ host (port of GenericAI.App)
    main.cpp                    Program.cs: startup / signal / cleanup order
    command_line_args.*         CommandLineArgs.cs
    http_control_server.*       HttpListenerHost.cs (/Alive /GetLicense /GetSettingsSchema /SetParameters)
    settings_schema.*           SettingsSchema.cs
    pipeline.*                  FrameDispatcher / EncodeWorker / SendWorker / ZmqResultSender
    channel_handle.*            ChannelHandle.cs (queues, trigger throttle, send parking)
    console_log.*, file_logger.*, state.h, blocking_queue.h, gai_native_api.h
  native/                     GenericAI.Native sources, ported (see "Native changes")
  tests/
    CppUnitTest.h, test_main.cpp  shim so the GenericAI.Native.Tests sources run unchanged
    test_*.cpp                    copied from GenericAI.Native.Tests
    simulator.py                  recorder-side simulator (ZMQ + MMF)
    run_e2e.sh                    end-to-end test runner
  scripts/
    install_deps_ubuntu.sh
    fetch_onnxruntime.sh
```

The YOLOX model is not duplicated: CMake copies it from `../GenericAI_ZMQ/native-deps/models/` (override with `-DGAI_MODEL_DIR=`).

## Build

Tested on Ubuntu 22.04 (GCC 11, CMake 3.22, FFmpeg 4.4, libzmq 4.3.4, libjpeg-turbo 2.1.2) with onnxruntime 1.20.1.

```bash
scripts/install_deps_ubuntu.sh          # apt: build tools, libzmq, FFmpeg, turbojpeg, json, httplib
scripts/fetch_onnxruntime.sh            # onnxruntime 1.20.1 GPU package -> third_party/onnxruntime
                                        # (scripts/fetch_onnxruntime.sh cpu  for the CPU-only package)
cmake -S . -B build -G Ninja            # -DONNXRUNTIME_ROOT=/path/to/onnxruntime if elsewhere
cmake --build build
ctest --test-dir build                  # native unit tests
```

Output is a self-contained folder, `build/GenericAI/`:

```text
GenericAI  GenericAI.Config  libonnxruntime.so.1  libonnxruntime_providers_{shared,cuda}.so  models/
```

The executable has `RPATH=$ORIGIN`, so the folder can be copied anywhere (e.g. next to the recorder as `GenericAI/`). `cmake --install build --prefix /opt/spark` installs the same folder.

CMake options: `GAI_ENABLE_ZMQ` (default ON; OFF = MMF + HTTP only, no libzmq/FFmpeg), `GAI_BUILD_TESTS` (default ON). If the system has no CMake package for cpp-httplib (Ubuntu's `libcpp-httplib-dev` 0.10 ships none) or nlohmann_json, CMake downloads a pinned version.

### GPU (CUDA)

The Person / object-detection detector tries the CUDA EP first and falls back to CPU (`kPreferGpu`, `kCudaDeviceId` in `native/gai_config.h`). With the onnxruntime 1.20 GPU package, CUDA needs, at runtime:

- an NVIDIA driver,
- CUDA 12 runtime libraries (`libcudart.so.12`, `libcublas(Lt).so.12`, `libcufft.so.11`, `libcurand.so.10`) and cuDNN 9,

on the loader path (system install, or `LD_LIBRARY_PATH`). When they are missing the log says why and the detector runs on CPU:

```text
[WARN] [native] [AI] CUDA EP unavailable (... libcublasLt.so.12: cannot open shared object file ...). Using CPU.
[INFO] detector backend = CPU
```

A working GPU shows `detector backend = CUDA(0)`. Motion detection is CPU-only on both platforms.

Without a system CUDA install, the NVIDIA pip wheels are enough (this is how it was tested, WSL2 + RTX 3050):

```bash
python3 -m venv /opt/cudart
/opt/cudart/bin/pip install "nvidia-cudnn-cu12==9.*" nvidia-cublas-cu12 nvidia-cuda-runtime-cu12 \
    nvidia-cufft-cu12 nvidia-curand-cu12 nvidia-cuda-nvrtc-cu12
export LD_LIBRARY_PATH=$(ls -d /opt/cudart/lib/python3*/site-packages/nvidia/*/lib | tr '\n' ':')$LD_LIBRARY_PATH
# WSL2 only: the driver's libcuda.so lives in /usr/lib/wsl/lib
```

Measured on the same 640x360 / 15 fps clip: CUDA analysed 225/225 frames, CPU (8 intra-op threads) 103/225.

## Run

Arguments, defaults, exit codes, and the transport selection rules are identical to the Windows version — see [`../GenericAI_ZMQ/README.md` → Run](../GenericAI_ZMQ/README.md#run). The executable is `GenericAI` instead of `GenericAI.exe`:

```bash
# ZMQ (recorder BINDs the sockets, wrapper connects)
./GenericAI port=46000 mode=single channel_count=4 detector=objectdetection \
    server_ip=172.20.1.36 result_port=9905 stream_port=9906

# MMF + HTTP (local; frames from /dev/shm/ChannelFrame_<port>)
./GenericAI port=46000 detector=motion
```

- `http_host=+` (or `*`) binds every interface; no URL reservation is needed on Linux.
- SIGINT / SIGTERM / SIGHUP shut down cleanly (same order as the C# `Cleanup()`), exit code 0.

### Linux-specific behaviour

- **`/SetParameters` answers 503 until the detector has finished loading** (same as the Windows host since 2026-10-07). Answering 200 in that window dropped the parameters — native had no channels yet — and the built-in default `ai_settings` then replaced them. The recorder keeps resending until it gets a 200 (`m_needSendSetting`), so the 503 makes it retry instead of losing its ROIs/settings.
- In a send-retry, a channel stays blocked while another worker is retrying its parked payload (the C# version only checked the parking queue, contrary to its own comment).

## Logging

`GenericAI.Config` (next to the executable) has the same switches as on Windows: `show_debug`, `show_native_debug`, `log_to_file`. Linux adds `log_dir`. The log directory is the first of these that can be created and written:

1. `log_dir` in `GenericAI.Config`, else `$GENERICAI_LOG_DIR`
2. `/var/log/spark/GenericAI`
3. `<exe dir>/Logs`

Files: `GenericAI-<basePort>.log`, `error-<basePort>.log`; 5 MB rotation with 3 backups. Native lines arrive with a `[native]` prefix. The C# `TimingRecorder` (compile-time off on Windows) is not ported; the native `kEnableTimingLog` still works.

## Tests

```bash
ctest --test-dir build                                       # 35 native unit tests
apt-get install -y python3-zmq ffmpeg                        # simulator deps
tests/run_e2e.sh build/GenericAI                             # end-to-end
tests/run_e2e.sh build/GenericAI people.mp4                  # + object detection must recognise something
```

`run_e2e.sh` starts the wrapper for each case, drives it with `tests/simulator.py` (which plays the recorder), then stops it with SIGTERM:

| Case | Checks |
| --- | --- |
| args | bad value / unknown key / incomplete `server_ip` shorthand → exit 2; port in use → exit 3 |
| zmq-motion | 2 channels, H.264 over the ZMQ frame plane, results over the ZMQ result plane |
| mmf-motion | I420 through `/dev/shm/ChannelFrame_<port>` (recorder layout), results by HTTP POST |
| zmq-objdet | YOLOX; with a video, at least one class must be recognised |
| every case | `/Alive`, `/GetSettingsSchema` keys, `/SetParameters` (and 400 for a v1.2 body without `jpg_compress`), each result's version / port_num / JPEG keyframe / echoed timestamp / `rois_rects`; exit 0 on SIGTERM |

The simulator can also be used by hand against a running wrapper: `python3 tests/simulator.py zmq --help`.

## Native changes vs `GenericAI_ZMQ/GenericAI.Native`

Everything not listed is a verbatim copy.

| File | Change |
| --- | --- |
| `pch.h`, `gai_platform.h` (new) | no `<windows.h>`; `GAI_EXPORT` / `GAI_CDECL` / `GAI_STDCALL` macros |
| `gai_abi.h` | callbacks use `GAI_STDCALL` |
| `exports.cpp` | `GAI_EXPORT`; exe dir from `/proc/self/exe`; `/` path join |
| `mmf_reader.*` | `shm_open` + `fstat` size check + `mmap` instead of `OpenFileMapping` / `MapViewOfFile` |
| `detector_person.*` | CUDA EP via `OrtApi::SessionOptionsAppendExecutionProvider_CUDA` instead of DirectML; UTF-8 model path (no `Widen`); fallback reason also goes to the host log |
| `gai_config.h` | `kCudaDeviceId`; comments |

When fixing a bug in the shared native code, apply it to both copies.

## Recorder integration

- **Spawn path**: AIService builds it with `GenericAIDevice::buildInExePath()` = `<recorder dir>/GenericAI/GenericAI` on Linux (`GenericAI\GenericAI.exe` on Windows). The same helper drives the "AI executable missing" check and `TerminateProcessesByName` (which matches `/proc/<pid>/comm` = `GenericAI`).
- **Packaging**: `spark.recorder/autobuild_argo.sh` builds this folder after AIService (`deploy_genericai`) and copies `build/GenericAI/` to `_bin-linux/GenericAI/`, so it ends up in the Docker image / systemd install next to the recorder. It is built on the recorder's build host on purpose: the binary links that distro's libzmq / FFmpeg SONAMEs. Knobs: `GENERICAI_SRC` (default `../GenericAISampleCode/GenericAI_Linux`), `ORT_FLAVOR=gpu|cpu`, `ORT_VERSION`, `ONNXRUNTIME_ROOT`, `SKIP_GENERICAI=1`. The image needs the `libturbojpeg` runtime package (added to `deploy/Dockerfile`).
- Uploaded **external** AIs (`genericai_detection`) are still unzipped with PowerShell `Expand-Archive` (`AIServiceModule.cpp`), so that path remains Windows-only.

## Known issues on the recorder side (not fixed here)

- `MMF_Data_Generic::image_data` in `modules/AIService/mmf.h` is declared `unsigned char* image_data[1920*1080*3]` (an array of pointers), so the object is 8x larger than intended and its footer is not where the wrapper's is. Frames still line up because `image_data` starts at the same offset on both sides; the wrapper's layout matches the intended one.
