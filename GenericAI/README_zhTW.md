# GenericAI

多通道 AI wrapper，搭配 Spark Recorder 的 `AStreamPerimeter_GenericAI` AI 服務模組使用。單一 process 服務 N 路互相獨立的影像通道，共用同一個 detector backend。

English version：see [README.md](README.md).

## 元件

| 子專案 | 類型 | 產出 | 用途 |
| --- | --- | --- | --- |
| `GenericAI.App` | C# (.NET Framework 4.8) | `GenericAI.exe` | Host process：HTTP listener、MMF reader、JPEG encode 與 HTTP send worker pool、native interop。 |
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

或在 Visual Studio 開啟 `GenericAI/GenericAI.sln`、選 `x64 / Release`、build。產出位於 `GenericAI/bin/Release/x64/`。

## 發佈內容

Release build 後，部署所需的所有檔案都在 `GenericAI/bin/Release/x64/`：

| 檔案 | 用途 |
| --- | --- |
| `GenericAI.exe` + `GenericAI.exe.config` | Host process。 |
| `GenericAI.Native.dll` | Detector pipeline（`GAI_*` C ABI）。 |
| `onnxruntime.dll`、`DirectML.dll` | Person detector 用的 ONNX Runtime + DirectML EP。 |
| `turbojpeg.dll` + `turbojpeg.LICENSE.md` | Callback JPEG encode（授權檔須隨 dll 一起出貨）。 |
| `Newtonsoft.Json.dll`、`System.Buffers.dll` | Managed 相依。 |
| `models\`（`yolox_m_fp16.onnx` + `LICENSE.md`） | Person detector 模型（Apache 2.0 授權文字須隨附）。 |

`*.pdb` / `*.lib` / `*.exp` / `*.iobj` / `*.ipdb` 為建置中間產物，不需出貨。

## 執行

```
GenericAI.exe [port=<X>] [channel_count=<N>] [encode_workers=<E>] [send_workers=<S>]
```

| 參數 | 預設 | 意義 |
| --- | --- | --- |
| `port` | `51000` | Base sample / callback port。第 `k` 路（從 0 起算）綁 `port + 2*k`。 |
| `channel_count` | `1` | 本 process 服務的 channel 數量。不設上限，`<1` 視為非法。 |
| `encode_workers` | `2` | JPEG encode worker thread 數（process-wide pool，跨 channel 共用）。 |
| `send_workers` | `2` | HTTP POST worker thread 數（process-wide pool）。 |

Exit code：

| Code | 意義 |
| --- | --- |
| `0` | OK |
| `1` | 一般失敗 |
| `2` | 參數錯誤 |
| `3` | Port 已被佔用 |
| `4` | Native init 失敗（detector / ONNX session / MMF） |

正式部署一律明示帶 `port=`。不帶任何參數直接執行時走預設 `port=51000`（「Debug Run」模式），方便在 Visual Studio 直接按 F5。

## 日誌與診斷

所有開關都是編譯期常數：改一行、rebuild、重啟 exe。維持下表預設值時，console 輸出與舊版 `CSharp/SampleWrapper.exe` 一致，也不會建立任何 log 目錄。

| 開關 | 位置 | 預設 | 效果 |
| --- | --- | --- | --- |
| `FileLogger.Enabled` | `GenericAI.App/Diagnostics/FileLogger.cs` | `false` | INFO / WARN / ERROR 檔案日誌，含經由 log callback 轉送的 native 端訊息。 |
| `ConsoleLog.Enabled` | `GenericAI.App/Diagnostics/ConsoleLog.cs` | `true` | 與舊版 sample 對齊的 console 輸出（HTTP 狀態、SetParameters echo、送出結果）。 |
| `TimingRecorder.Enabled` | `GenericAI.App/Diagnostics/TimingRecorder.cs` | `false` | C# 側 per-frame timing 輸出（console + `timing-<basePort>.log`），同時打開 GenericAI 特有的 verbose console 訊息。 |
| `kEnableTimingLog` | `GenericAI.Native/gai_config.h` | `false` | Native 側 per-frame timing 輸出與 native 端的資訊性 console 訊息。 |

Log 檔位於 `%ProgramData%\Spark\GenericAI\Logs\`：

- `GenericAI-<basePort>.log` — INFO / WARN / ERROR
- `error-<basePort>.log` — ERROR 另存一份，方便快速排查
- `timing-<basePort>.log` — C# `TimingRecorder` 的 per-frame timing 輸出

寫入為非同步：producer 只把訊息塞進 bounded queue，由單一背景 thread 批次落盤，所以日誌絕不會卡住 frame 路徑。檔案 5 MB 輪替、保留 3 份備份；檔名帶 base port，多個 instance 並行時不會搶同一個檔案。

檔案日誌開啟時，native 端的診斷訊息（pool 配置、MMF 對應、丟 frame 警告）會帶 `[native]` 前綴進到同一個 per-port log 檔：host 啟動時透過 `GAI_RegisterLogCallback` 註冊 sink，managed / native 兩側的事件都收在同一份檔案裡。

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
- `image_size` 可能大於緊湊排列的 I420 實際大小（recorder 解碼緩衝區以 32-byte 對齊的 plane stride 計算，例如 352x240 回報 130560 bytes，實際影像只有 126720 bytes）。Wrapper 以影格自帶的寬高算出 `width*height*3/2` 來驗證與複製，容忍多出來的餘量。

## Detector backend

由 `gai_config.h` 編譯期決定要載入哪一個 detector：

```cpp
constexpr DetectorKind kDetectorKind = DetectorKind::Motion;   // 或 DetectorKind::Person
constexpr bool         kPreferGpu    = true;                   // 僅 Person 有效
```

Process 內所有 channel 共用同一支 detector instance，透過 `SharedDetectorScheduler` 排程，結果由 `FrameDispatcher` 路由回原 channel。`kEnablePipelinedInference` 開啟（預設）且 detector 支援 pipelined inference 時（Person 支援、Motion 不支援），推論會拆成「CPU 預處理」與「GPU 推論 + 後處理」兩條 loop，中間夾一個小 queue，讓 CPU 跟 GPU 工作跨 frame 重疊。

### Person — YOLOX-M FP16 ONNX

- DirectML EP，失敗時自動 fallback 到 CPU。實際載入的 EP 可透過 `GAI_GetBackend` 讀回；timing/verbose 開關打開時，啟動也會在 console 印出（`EP=DirectML(0)` 或 `EP=CPU`）。
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

`POST /SetParameters` 帶的是 per-ROI 的 `sensitivity[]` / `threshold[]` 陣列，但這組調參在語意上是**整個通道共用一組**：Native 端會挑第一個 polygon 有效（`rects.size() >= 3`）的 ROI slot，用它的 `(threshold, sensitivity)` 作為整個 frame 推論用的參數。如果整批 ROI 都不合法，保留上次的值；在第一次 `/SetParameters` 到達前，套用內建預設（`threshold=25`、`sensitivity=50`）。

### ROI 座標空間

`/SetParameters` 裡的 ROI 座標是以該 payload 的 `image_width × image_height` 為基準空間。從 MMF 讀進來的影格帶有自己的解析度，兩者可能不同（例如同一支攝影機的子串流）。每次推論前 scheduler 會把 ROI 矩形——以及 Motion 回呼回聲的 polygon 點——等比換算到實際影格解析度，因此不論串流解析度為何，同一個 ROI 都對應到畫面上同一塊區域。回呼裡的座標一律是**實際影格的像素空間**，與隨附的 keyframe JPEG 對齊。

每通道的 frame pool 仍以第一次 `/SetParameters` 的 `image_width`/`image_height` 配置容量並在通道存活期間鎖定：**大於**該容量的影格會在進偵測前被丟棄（console 會輸出節流後的警告），之後再送不同解析度的 `/SetParameters` 也會被忽略並警告。要換成更大的解析度需重啟該通道。

## Native ABI

`GenericAI.Native.dll` 對外匯出一組小型 C ABI，由 `GenericAI.App` 透過 P/Invoke 取用（`NativeInterop.cs`）：

| Symbol | 用途 |
| --- | --- |
| `GAI_InitializeChannels(ports, count)` | 配置 per-channel pipeline、建立 detector、啟動 scheduler。 |
| `GAI_SetChannelParameters(port, *GAI_Settings)` | 將最新的 `/SetParameters` payload 推進對應 channel。 |
| `GAI_RegisterCallback(cb)` | 註冊 `FrameDispatcher` 觸發的偵測 callback。 |
| `GAI_RegisterLogCallback(cb)` | 註冊 host log sink；native 端 INFO / WARN / ERROR 訊息帶 `[native]` 前綴進 per-port log 檔。 |
| `GAI_GetBackend(buf, len)` | 讀回實際載入的 EP（`CPU` / `DirectML(0)`）。 |
| `GAI_Deinitialize()` | 停 scheduler、釋放 detector、清 queue。 |

## 與 `CSharp/` 的關係

`CSharp/` 是舊版單通道 sample wrapper（`SampleWrapper.exe` + `SampleDLL.dll`），保留作為參考程式碼。GenericAI 用於正式部署、取代它：

| | `CSharp/SampleWrapper.exe` | `GenericAI.exe` |
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
    Http/
      HttpListenerHost.cs   /Alive、/GetLicense、/SetParameters
      HttpPostClient.cs     detection callback 共用的 HttpClient
      HttpEnvelope.cs       v1.2 callback JSON payload
      ParameterStore.cs     來自 /SetParameters 的 process-wide url + jpgQuality 快取
    Pipeline/
      FrameDispatcher.cs    detector callback → channel router
      EncodeWorker.cs       JPEG encode worker（turbojpeg）
      SendWorker.cs         HTTP POST worker
      RoundRobinTaker.cs    worker pool 共用的公平多佇列取出器
      DropCounter.cs        各 pipeline 階段的 drop 計數
    Interop/
      NativeInterop.cs      GAI_* exports 的 P/Invoke 表面
      TurboJpegInterop.cs   turbojpeg 的 P/Invoke 表面
      DetectorType.cs       對應 native DetectorKind 的 managed 列舉
    Diagnostics/
      FileLogger.cs         非同步檔案 logger（%ProgramData%\Spark\GenericAI\Logs）
      ConsoleLog.cs         與舊版 sample 對齊的 console 輸出開關
      TimingRecorder.cs     可選的 timing instrumentation
  GenericAI.Native/         磁碟上維持扁平；VS 內以 .vcxproj.filters 分組
    exports.cpp             GAI_* C ABI
    host_log.{h,cpp}        native → host log 橋接（GAI_RegisterLogCallback）
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
