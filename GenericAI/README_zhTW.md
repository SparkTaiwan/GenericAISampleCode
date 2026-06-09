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
- 建議有 DirectML 相容的 GPU 跑 Person detector，找不到時自動 fallback 到 CPU。

第三方相依（透過 NuGet 還原）：
- `Microsoft.ML.OnnxRuntime.DirectML` 1.14.1 — Person detector inference。
- `Microsoft.AI.DirectML` 1.10.1。
- `Newtonsoft.Json` 13.0.3 — HTTP `POST /SetParameters` payload。
- `libjpeg-turbo`（位於 `native-deps/win-x64/turbojpeg.dll`）— callback JPEG encode。

Person detector 模型放在 `native-deps/models/yolox_m_fp16.onnx`（YOLOX-M、FP16、以 COCO 訓練，class 0 對應 person）。路徑相對 exe 解析。

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

正式部署一律明示帶 `port=`。不帶任何參數直接執行時走預設 `port=51000`（「Debug Run」模式），方便在 Visual Studio 直接按 F5。

## 對外契約

HTTP control plane 與 MMF layout 完全對齊 `spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp`，recorder 端無需改動。

### HTTP（recorder ↔ wrapper）

每個 channel 綁自己的 port：

| Endpoint | 方向 | 用途 |
| --- | --- | --- |
| `GET /Alive` | recorder → wrapper | 健康檢查，回 200 OK。 |
| `GET /GetLicense` | recorder → wrapper | License token endpoint。 |
| `POST /SetParameters` | recorder → wrapper | v1.2 JSON：`{analytics_event_api_url, image_width, image_height, jpg_compress, rois: [{sensitivity, threshold, rects: [{x,y}, ...]}]}`。`sensitivity` / `threshold` 在 Motion 跟 Person 兩種 backend 的解讀方式不同，詳見下面 **Detector backend**。 |
| `POST <analytics_event_api_url>` | wrapper → recorder | Detection callback，body 含 keyframe 的 JPEG 與 ROI metadata。 |

### MMF（recorder → wrapper）

- Per-channel 命名：`ChannelFrame_<channelPort>`（第 `k` 路讀 `ChannelFrame_{port + 2*k}`）。
- Layout 對齊舊版 `CSharp/SampleDLL/dllmain.cpp` 的 `MMF_Data`，讓 recorder 端 writer 不用改。
- `status` byte：`0=unused → 1=new frame → 2=consumed`。Wrapper poll 拿到後翻 `1 → 2`。

## Detector backend

由 `gai_config.h` 編譯期決定要載入哪一個 detector：

```cpp
constexpr DetectorKind kDetectorKind = DetectorKind::Person;   // 或 DetectorKind::Motion
constexpr bool         kPreferGpu    = true;                   // 僅 Person 有效
```

Process 內所有 channel 共用同一支 detector instance，透過 `SharedDetectorScheduler` 排程，結果由 `FrameDispatcher` 路由回原 channel。`kEnablePipelinedInference` 開啟（預設）且 detector 支援 pipelined inference 時（Person 支援、Motion 不支援），推論會拆成「CPU 預處理」與「GPU 推論 + 後處理」兩條 loop，中間夾一個小 queue，讓 CPU 跟 GPU 工作跨 frame 重疊。

### Person — YOLOX-M FP16 ONNX

- DirectML EP，失敗時自動 fallback 到 CPU。實際載入的 EP 在啟動時印 log（`EP=DirectML(0)` 或 `EP=CPU`），也能透過 `GAI_GetBackend` 讀回。
- Letterbox 預處理 → ONNX 推論 → score 過濾 + NMS → ROI overlap 過濾。
- Per-channel ROI 清單來自 `POST /SetParameters`。

**參數對應（每次呼叫從 `/SetParameters` 拿）：**

| 欄位 | 範圍 | 效果 |
| --- | --- | --- |
| `threshold` | 0..100 | 線性對應到 YOLOX confidence 門檻 `0.20 .. 0.70`。越高越嚴（誤報少、漏報多）。 |
| `sensitivity` | — | 不使用。YOLOX 推論只有一個有效旋鈕（confidence score），兩個 slider 會壓到同一條軸。此欄位仍保留在 JSON 中以維持與 Motion 共用的協定格式。 |

Detector 內部常數（不在對外協定中）：
- NMS IoU 門檻：建構時固定（0.45）。
- 目標類別：建構時固定（0 = COCO 的 person）。
- Frame decimation：`stride_n=1`（每幀都跑推論）。

### Motion — frame difference

- 跟前一張 frame 比 Y plane 的每像素差，做 sub-sampling，per-ROI 融合迴圈加上早期退出。
- 不用模型檔，純 CPU。

**參數對應（每次呼叫從 `/SetParameters` 拿）：**

| 欄位 | 範圍 | 效果 |
| --- | --- | --- |
| `threshold` | 0..100 | 對應到每像素差的門檻 `8 .. 40`（8-bit 灰階）。越高代表只看大幅度的亮度變化。 |
| `sensitivity` | 0..100 | 對應到觸發所需的最小變化像素比例 `0.20 .. 0.005`（佔該 ROI 自身面積）。越高代表需要的變化像素越少。 |

兩個軸彼此正交：`threshold` 控「每個像素要變化多大才算數」，`sensitivity` 控「要多少這種像素才觸發」。

### 參數傳遞流程

`POST /SetParameters` 帶的是 per-ROI 的 `sensitivity[]` / `threshold[]` 陣列。Native 端會挑第一個 polygon 有效（`rects.size() >= 3`）的 ROI slot，用它的 `(threshold, sensitivity)` 作為整個 frame 推論用的參數。如果整批 ROI 都不合法，保留上次的值；在第一次 `/SetParameters` 到達前，套用內建預設（`threshold=25`、`sensitivity=50`）。

## Native ABI

`GenericAI.Native.dll` 對外匯出一組小型 C ABI，由 `GenericAI.App` 透過 P/Invoke 取用（`NativeInterop.cs`）：

| Symbol | 用途 |
| --- | --- |
| `GAI_InitializeChannels(ports, count)` | 配置 per-channel pipeline、建立 detector、啟動 scheduler。 |
| `GAI_SetChannelParameters(port, *GAI_Settings)` | 將最新的 `/SetParameters` payload 推進對應 channel。 |
| `GAI_RegisterCallback(cb)` | 註冊 `FrameDispatcher` 觸發的偵測 callback。 |
| `GAI_GetBackend(buf, len)` | 讀回實際載入的 EP（`CPU` / `DirectML(0)`）。 |
| `GAI_Deinitialize()` | 停 scheduler、釋放 detector、清 queue。 |

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
    ChannelHandle.cs        per-channel 狀態（listener + MMF reader）
    NativeInterop.cs        GAI_* exports 的 P/Invoke 表面
    FrameDispatcher.cs      detector callback → channel router
    EncodeWorker.cs         JPEG encode worker（turbojpeg）
    SendWorker.cs           HTTP POST worker
    HttpListenerHost.cs     /Alive、/GetLicense、/SetParameters
    ParameterStore.cs       來自 /SetParameters 的 process-wide url + jpgQuality 快取
    FileLogger.cs           檔案 logger（D:\SLog-<basePort>）
    TimingRecorder.cs       可選的 timing instrumentation
  GenericAI.Native/
    exports.cpp             GAI_* C ABI
    shared_detector_scheduler.{h,cpp}   單 detector + N-channel 排程
    channel_pipeline.{h,cpp}            per-channel work queue
    param_snapshot.{h,cpp}              per-channel /SetParameters 儲存（ROI + 調校參數）
    detector_factory.{h,cpp}            編譯期挑 Motion 或 Person
    detector_motion.{h,cpp}             frame-diff detector
    detector_person.{h,cpp}             YOLOX-M FP16 detector（DirectML / CPU）
    mmf_reader.{h,cpp}                  per-channel MMF poll loop
    gai_abi.h / gai_config.h            ABI 宣告 + 編譯期 flag
  native-deps/
    models/yolox_m_fp16.onnx
    win-x64/turbojpeg.dll + LICENSE.md + VERSION.txt
```
