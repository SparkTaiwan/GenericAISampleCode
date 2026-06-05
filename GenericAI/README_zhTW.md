# GenericAI

多通道 AI wrapper，搭配 Spark Recorder 的 `AStreamPerimeter_GenericAI` AI 服務模組使用。單一 process 服務 N 路互相獨立的影像通道，共用同一個 detector backend。

English version：see [README.md](README.md).

## 元件

| 子專案 | 類型 | 產出 | 用途 |
| --- | --- | --- | --- |
| `GenericAI.App` | C# (.NET Framework 4.8) | `GenericAI.App.exe` | Host process：HTTP listener、MMF reader、JPEG encode 與 HTTP send worker pool、native interop。 |
| `GenericAI.Native` | C++ (MSVC v140) | `GenericAI.Native.dll` | Detector 實作 + per-channel pipeline，匯出 `GAI_*` C ABI。 |

## 需求

- Windows 10 / 11，x64。
- Visual Studio 2019 或 2022，需安裝 **MSVC v140 toolset** 與 **Windows 8.1 SDK**（C++ 子專案目標 v140，與 Spark Recorder 的 link 環境一致）。
- .NET Framework **4.8** Developer Pack。
- 建議有 DirectML 相容的 GPU 跑 person detector，找不到時自動 fallback 到 CPU。

第三方相依（透過 NuGet 還原）：
- `Microsoft.ML.OnnxRuntime.DirectML` 1.14.1 — person detector inference。
- `Microsoft.AI.DirectML` 1.10.1。
- `Newtonsoft.Json` 13.0.3 — HTTP `POST /SetParameters` payload。
- `libjpeg-turbo`（位於 `native-deps/win-x64/turbojpeg.dll`）— callback JPEG encode。

Person detector 模型放在 `native-deps/models/yolox_m_fp16.onnx`（YOLOX-M、FP16），路徑相對 exe 解析。

## 建置

```powershell
# 從 repo 根目錄
nuget restore GenericAI/GenericAI.sln
msbuild GenericAI/GenericAI.sln /p:Configuration=Release /p:Platform=x64
```

或在 Visual Studio 開啟 `GenericAI/GenericAI.sln`、選 `x64 / Release`、build。產出位於 `GenericAI/bin/Release/`。

## 執行

```
GenericAI.App.exe [port=<X>] [channel_count=<N>] [encode_workers=<E>] [send_workers=<S>] [log=<dir>]
```

| 參數 | 預設 | 意義 |
| --- | --- | --- |
| `port` | `51000` | Base sample / callback port。第 `k` 路（從 0 起算）綁 `port + 2*k`。 |
| `channel_count` | `1` | 本 process 服務的 channel 數量。不設上限，`<1` 視為非法。 |
| `encode_workers` | `2` | JPEG encode worker thread 數（process-wide pool，跨 channel 共用）。 |
| `send_workers` | `2` | HTTP POST worker thread 數（process-wide pool）。 |
| `log` | `""`（自動） | 覆寫 log 目錄。預設為 `D:\SLog-<basePort>\GenericAI.log`。 |

Exit code：

| Code | 意義 |
| --- | --- |
| `0` | OK |
| `1` | 一般失敗 |
| `2` | 參數錯誤 |
| `3` | Port 已被佔用 |
| `4` | Native init 失敗（detector / ONNX session / MMF） |

正式部署一律明示帶 `port=`。不帶任何參數直接執行時走預設 `port=51000`（"Debug Run" 模式），方便 Visual Studio 直接按 F5。

## 對外契約

HTTP control plane 與 MMF layout 完全對齊 `spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp`，recorder 端無需改動。

### HTTP（recorder ↔ wrapper）

每個 channel 綁自己的 port：

| Endpoint | 方向 | 用途 |
| --- | --- | --- |
| `GET /Alive` | recorder → wrapper | 健康檢查，回 200 OK。 |
| `GET /GetLicense` | recorder → wrapper | License token endpoint。 |
| `POST /SetParameters` | recorder → wrapper | v1.2 JSON：`{analytics_event_api_url, image_width, image_height, jpg_compress, rois: [{sensitivity, threshold, rects: [{x,y}, ...]}]}`。 |
| `POST <analytics_event_api_url>` | wrapper → recorder | Detection callback，body 含 keyframe 的 JPEG 與 ROI metadata。 |

### MMF（recorder → wrapper）

- Per-channel 命名：`ChannelFrame_<channelPort>`（第 `k` 路讀 `ChannelFrame_{port + 2*k}`）。
- Layout 對齊舊版 `CSharp/SampleDLL/dllmain.cpp` 的 `MMF_Data`，讓 recorder 端 writer 不用改。
- `status` byte：`0=unused → 1=new frame → 2=consumed`。Wrapper poll 拿到後翻 `1 → 2`。

## Detector backend

由 `gai_config.h` 編譯期決定：

- **Person** — YOLOX-M FP16 ONNX，DirectML EP 失敗時 fallback 到 CPU。Letterbox preprocess + NMS + ROI overlap filter。Per-channel ROI 清單來自 `POST /SetParameters`。
- **Motion** — frame-diff 加 per-pixel threshold，per-ROI 融合迴圈附早期退出。設定旋鈕與舊版 `CSharp/SampleDLL` motion detector 一致。

Process 內所有 channel 共用同一支 detector instance，透過 `SharedDetectorScheduler` 排程；channel 輪流送 frame、結果由 `FrameDispatcher` 路由回原 channel。

## 與 `CSharp/` 的關係

`CSharp/` 是舊版單通道 sample wrapper（`SampleWrapper.exe` + `SampleDLL.dll`），保留作為參考程式碼。GenericAI 用於正式部署、取代它：

| | `CSharp/SampleWrapper.exe` | `GenericAI.App.exe` |
| --- | --- | --- |
| 每 process channel 數 | 1 | N（單 process 多通道） |
| Detector | 只有 Motion | Motion 或 Person（YOLOX） |
| 產出 DLL | `SampleDLL.dll`（非 `GAI_*` exports） | `GenericAI.Native.dll`（`GAI_*` exports） |
| Recorder 端 cmdline 改動 | 無 | 增加 `channel_count=N` |

Recorder 端透過 `AStreamPerimeter_GenericAI` 決定要 spawn 哪支 exe。兩支可同時放在硬碟上、不共用 DLL、不衝突。

## 專案結構

```
GenericAI/
  GenericAI.sln
  GenericAI.App/
    Program.cs              進入點 + cleanup 編排
    CommandLineArgs.cs      cmdline 解析
    ChannelHandle.cs        per-channel 狀態（listener / MMF reader / param store）
    NativeInterop.cs        GAI_* exports 的 P/Invoke 表面
    FrameDispatcher.cs      detector callback → channel router
    EncodeWorker.cs         JPEG encode worker（turbojpeg）
    SendWorker.cs           HTTP POST worker
    HttpListenerHost.cs     /Alive、/GetLicense、/SetParameters
    ParameterStore.cs       每個 channel 最近一筆 SetParameters
    FileLogger.cs           檔案 logger（D:\SLog-<basePort>）
    TimingRecorder.cs       可選的 timing instrumentation
  GenericAI.Native/
    exports.cpp             GAI_* C ABI
    shared_detector_scheduler.{h,cpp}   單 detector + N-channel 排程
    channel_pipeline.{h,cpp}            per-channel work queue
    detector_factory.{h,cpp}            編譯期挑 Motion 或 Person
    detector_motion.{h,cpp}             frame-diff detector
    detector_person.{h,cpp}             YOLOX-M FP16 detector（DirectML / CPU）
    mmf_reader.{h,cpp}                  per-channel MMF poll loop
    gai_abi.h / gai_config.h            ABI 宣告 + 編譯期 flag
  native-deps/
    models/yolox_m_fp16.onnx
    win-x64/turbojpeg.dll + LICENSE.md + VERSION.txt
```
