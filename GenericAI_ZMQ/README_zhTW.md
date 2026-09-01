# GenericAI (ZMQ)

多通道 AI wrapper，搭配 Spark Recorder 的 `AStreamPerimeter_GenericAI` AI 服務模組使用。單一 process 服務 N 路互相獨立的影像通道，共用同一個 detector backend。

支援兩種傳輸方式，於啟動時（見 **執行**）決定：

- **ZMQ（正式部署預設）**— recorder 透過 ZMQ frame plane PUSH 已編碼的 H.264/H.265 access unit，wrapper 解碼後把分析結果透過 ZMQ result plane PUSH 回去。可本機、也可遠端。
- **MMF + HTTP（舊版 / 僅限本機）**— recorder 把原始 I420 影格寫進 per-channel 共享記憶體，wrapper 以 HTTP POST 回傳結果。保留作向後相容；只有在沒帶任何 ZMQ endpoint 時才會走這條。

English version：see [README.md](README.md).

## 元件

| 子專案 | 類型 | 產出 | 用途 |
| --- | --- | --- | --- |
| `GenericAI.App` | C# (.NET Framework 4.8) | `GenericAI.exe` | Host process：HTTP 控制 listener、ZMQ result sender / MMF reader、JPEG encode 與 send worker pool、native interop。 |
| `GenericAI.Native` | C++ (MSVC v140) | `GenericAI.Native.dll` | Detector 實作、per-channel pipeline、ZMQ frame receiver + NAL（H.264/H.265）解碼，匯出 `GAI_*` C ABI。 |

## 需求

- Windows 10 / 11，x64。
- Visual Studio 2019 或 2022，需安裝 **MSVC v140 toolset** 與 **Windows 8.1 SDK**（C++ 子專案目標 v140，與 Spark Recorder 的 link 環境一致）。
- .NET Framework **4.8** Developer Pack。
- 建議有 DirectML 相容的 GPU 跑 Person detector，找不到時自動 fallback 到 CPU。

第三方相依（透過 NuGet 還原）：

| 套件 | 版本 | 使用者 | 用途 |
| --- | --- | --- | --- |
| `Newtonsoft.Json` | 13.0.3 | App | `/SetParameters` 解析 + 結果 JSON。 |
| `System.Buffers` | 4.5.1 | App | 影格路徑上的 pooled buffer。 |
| `Microsoft.ML.OnnxRuntime.DirectML` | 1.14.1 | Native | Person detector 推論。 |
| `Microsoft.AI.DirectML` | 1.10.1 | Native | DirectML execution provider。 |
| `libzmq-vc143` | 4.3.5 | Native | ZMQ frame + result 傳輸。 |
| `FFmpeg-lgpl3` | 4.4.1 | Native | H.264 / H.265 NAL 解碼（ZMQ frame plane）。 |
| `libjpeg-turbo`（位於 `native-deps/win-x64/turbojpeg.dll`） | — | App | Callback JPEG encode。 |

Person detector 模型放在 `native-deps/models/yolox_m_fp16.onnx`（YOLOX-M、FP16、以 COCO 訓練，class 0 對應 person）。路徑相對 exe 解析。

## 建置

```powershell
# 從 GenericAI_ZMQ 資料夾（solution 根目錄）
nuget restore GenericAI.sln
msbuild GenericAI.sln /p:Configuration=Release /p:Platform=x64
```

或在 Visual Studio 開啟 `GenericAI.sln`、選 `x64 / Release`、build。產出位於 `bin/Release/x64/`。

## 發佈內容

Release build 後，部署所需的所有檔案都在 `bin/Release/x64/`：

| 檔案 | 用途 |
| --- | --- |
| `GenericAI.exe` + `GenericAI.exe.config` | Host process。 |
| `GenericAI.Native.dll` | Detector pipeline + ZMQ receiver + NAL 解碼（`GAI_*` C ABI）。 |
| `onnxruntime.dll`、`DirectML.dll` | Person detector 用的 ONNX Runtime + DirectML EP。 |
| `turbojpeg.dll` + `turbojpeg.LICENSE.md` | Callback JPEG encode（授權檔須隨 dll 一起出貨）。 |
| `libzmq-v143-mt-4_3_5.dll`、`libsodium.dll` | ZMQ 傳輸（frame + result plane）。 |
| `avcodec-58.dll`、`avformat-58.dll`、`avutil-56.dll`、`swscale-5.dll`、`swresample-3.dll`、`avfilter-7.dll`、`avdevice-58.dll`、`postproc-55.dll` | FFmpeg — ZMQ 影格的 H.264/H.265 解碼。 |
| `Newtonsoft.Json.dll`、`System.Buffers.dll` | Managed 相依。 |
| `models\`（`yolox_m_fp16.onnx` + `LICENSE.md`） | Person detector 模型（Apache 2.0 授權文字須隨附）。 |

`*.pdb` / `*.lib` / `*.exp` / `*.iobj` / `*.ipdb` 為建置中間產物，不需出貨。

## 執行

### 1. 命令列參數

```
GenericAI.exe port=<int> [channel_count=<N>] [mode=single|multi] [detector=motion|objectdetection]
              [server_ip=<recorderIP> result_port=<port> stream_port=<port>]
              [frame_endpoint=tcp://host:port] [result_endpoint=tcp://host:port]
              [http_host=<ip|+>] [encode_workers=<N>] [send_workers=<M>]
```

參數是 `key=value`、順序不拘。未知的 key、缺少 `=`、或超出範圍的值都會以 exit code `2` 結束。

| 參數 | 預設 | 意義 |
| --- | --- | --- |
| `port` | `46000` | Base HTTP **控制** port（`/Alive`、`/SetParameters`、`/GetLicense`、`/GetSettingsSchema`）。第 `k` 路（從 0 起算）綁 `port + k`。 |
| `channel_count` | `1` | 本 process 服務的 channel 數量。不設上限，`<1` 視為非法。 |
| `mode` | `multi` | `single` = 本 process 服務 `channel_count` 路；`multi` = 一 process 一路。純資訊性（實際拓撲由 `channel_count` 決定），接受它只是為了不讓 recorder 傳的 `mode=` 被拒。 |
| `detector` | *（編譯期預設：Motion）* | `motion` 或 `objectdetection`（別名：`person`、`objdetection`）。於執行期覆寫 `gai_config.h` 的預設。省略則用編譯預設。 |
| `server_ip` + `result_port` + `stream_port` | *（未設）* | 簡寫，展開成 `result_endpoint=tcp://server_ip:result_port` 與 `frame_endpoint=tcp://server_ip:stream_port`。**三者須一起給。** `result_port` 用 recorder 的 `http_server_port`，`stream_port` 用它的 `ai_stream_port`（兩者獨立）。 |
| `frame_endpoint` | *（未設）* | 明確的 ZMQ frame plane 位址，wrapper 從這裡 PULL（例：`tcp://127.0.0.1:5556`）。優先於簡寫。未設 → 走 MMF 影格。 |
| `result_endpoint` | *（未設）* | 明確的 ZMQ result plane 位址，wrapper 往這裡 PUSH。優先於簡寫。未設 → 走 HTTP POST 結果。 |
| `http_host` | `127.0.0.1` | HTTP 控制 port 的 bind 位址。Loopback = 僅本機。**遠端** wrapper 要傳本機可達的 IP（`http_host=172.20.1.18`）或 `+`/`*` 綁所有介面。綁非 loopback 需管理員權限或 URL 保留（`netsh http add urlacl`）。 |
| `encode_workers` | `2` | JPEG encode worker thread 數（process-wide pool，1..16）。 |
| `send_workers` | `2` | 結果送出 worker thread 數（process-wide pool，1..16）。 |

**傳輸方式由「帶了哪些 endpoint」決定：**

- 同時設 `frame_endpoint` 與 `result_endpoint` → 全 ZMQ（影格用 ZMQ 進、結果用 ZMQ 出）。
- 都不設 → 舊版 MMF 影格 + HTTP 結果 POST。
- 兩個 plane 各自獨立，所以只設一個時可混用（例如 ZMQ 影格 + HTTP 結果）。

### 2. 單機 / 除錯執行

不帶任何參數（或在 Visual Studio 按 F5）會用 `port=46000`、單通道、MMF+HTTP、編譯預設 detector，適合快速冒煙測試：

```powershell
.\GenericAI.exe
```

指定 detector、4 個通道，走 MMF+HTTP：

```powershell
.\GenericAI.exe port=46000 channel_count=4 detector=objectdetection
```

對接一台在 `172.20.1.36` BIND 了 ZMQ socket 的 recorder（result plane 用它的 `http_server_port`、frame plane 用它的 `ai_stream_port`），用簡寫：

```powershell
.\GenericAI.exe port=46000 mode=single channel_count=4 detector=objectdetection `
    server_ip=172.20.1.36 result_port=9905 stream_port=9906
```

### 3. Recorder 如何 spawn（正式部署）

要用哪支 exe、帶什麼參數，由 recorder 的 `AStreamPerimeter_GenericAI` / `GenericAIDevice` 決定，一般不需要你手動啟動。這裡列出來只是方便你重現一次 spawn 來除錯。

**Single 模式 + ZMQ（內建 detector，`getUseZmq()==true`）**— recorder BIND 兩個 ZMQ socket，wrapper 撥 `127.0.0.1`：

```
GenericAI.exe port=<exe_server_port> mode=single channel_count=<N> detector=motion|objectdetection ^
              frame_endpoint=tcp://127.0.0.1:<ai_stream_port> ^
              result_endpoint=tcp://127.0.0.1:<http_server_port>
```

**Multi 模式（一 process 一路，MMF + HTTP）**— 外部 AI 只認得 `port=`，內建的另外會帶 `mode`/`detector`：

```
GenericAI.exe port=<channelPort> mode=multi detector=motion|objectdetection
```

備註：

- 不論哪種模式，HTTP 控制 port 都留在 `port=`（== `exe_server_port`，channel 0）；只有 frame/result **資料** plane 會搬到 ZMQ。
- **遠端** wrapper 不由 recorder spawn——由操作者在另一台主機手動啟動，帶 `http_host=<那台的 IP>`、以及指回 recorder 的 `server_ip`/endpoint 參數。
- ZMQ 需要 `mode=single`；若是 `mode=multi`，recorder 會記一筆警告且不啟用 ZMQ。

### 4. Exit code

| Code | 意義 |
| --- | --- |
| `0` | OK |
| `1` | 一般失敗 |
| `2` | 參數錯誤 |
| `3` | Port 已被佔用 |
| `4` | Native init 失敗（detector / ONNX session / ZMQ / MMF） |

**Degraded** init（detector/模型載入失敗，native 回 `5`）**不會**結束 process：process 續存、HTTP listener 續服務，`/Alive` 回 `{"status":"error","version":"...","message":"..."}`，讓 recorder 看到原因、而不是一直重啟。

## 日誌與診斷

所有開關都是編譯期常數：改一行、rebuild、重啟 exe。

| 開關 | 位置 | 預設 | 效果 |
| --- | --- | --- | --- |
| `FileLogger.Enabled` | `GenericAI.App/Diagnostics/FileLogger.cs` | `true` | INFO / WARN / ERROR 檔案日誌，含經由 log callback 轉送的 native 端訊息。 |
| `ConsoleLog.Enabled` | `GenericAI.App/Diagnostics/ConsoleLog.cs` | `true` | Console 狀態輸出（HTTP 狀態、SetParameters echo、送出結果）。 |
| `TimingRecorder.Enabled` | `GenericAI.App/Diagnostics/TimingRecorder.cs` | `false` | C# 側 per-frame timing 輸出（console + `timing-<basePort>.log`），同時打開 GenericAI 特有的 verbose console 訊息。 |
| `kEnableTimingLog` | `GenericAI.Native/gai_config.h` | `false` | Native 側 per-frame timing 輸出與 native 端的資訊性 console 訊息。 |

Log 檔位於 `%ProgramData%\Spark\GenericAI\Logs\`：

- `GenericAI-<basePort>.log` — INFO / WARN / ERROR
- `error-<basePort>.log` — ERROR 另存一份，方便快速排查
- `timing-<basePort>.log` — C# `TimingRecorder` 的 per-frame timing 輸出

寫入為非同步：producer 只把訊息塞進 bounded queue，由單一背景 thread 批次落盤，所以日誌絕不會卡住 frame 路徑。檔案 5 MB 輪替、保留 3 份備份；檔名帶 base port，多個 instance 並行時不會搶同一個檔案。

檔案日誌開啟時，native 端的診斷訊息（pool 配置、MMF 對應、解碼/ZMQ 錯誤、丟 frame 警告）會帶 `[native]` 前綴進到同一個 per-port log 檔：host 啟動時透過 `GAI_RegisterLogCallback` 註冊 sink，managed / native 兩側的事件都收在同一份檔案裡。

## 對外契約

HTTP control plane、MMF layout、ZMQ frame/result plane 皆對齊 `spark.recorder/modules/AIService/Generic/AStreamPerimeter_GenericAI.cpp` + `GenericAIDevice.cpp` + `ZmqFrameProtocol.h`，recorder 端無需改動。

### HTTP 控制平面（recorder ↔ wrapper）

每個 channel 的控制端點綁在自己的 port（`port + k`）。**不論哪種傳輸模式**這個 plane 都是 HTTP——只有 frame/result 資料 plane 會搬到 ZMQ。

| Endpoint | 方向 | 用途 |
| --- | --- | --- |
| `GET /Alive` | recorder → wrapper | 健康檢查。回 200，body `{"status":"ok","version":"1.3"}`，異常時 `{"status":"error","version":"1.3","message":"..."}`。recorder 讀 `version` 決定 `/SetParameters` 要用哪個協定版本（沒有此欄位時 fallback 為 `1.2`——舊版 wrapper）。 |
| `GET /GetLicense` | recorder → wrapper | License token endpoint（內建版回空 body）。 |
| `GET /GetSettingsSchema` | recorder → wrapper | 對應執行中 detector（motion vs object detection）的自描述 AI 設定 schema（spec §5）。ConfigClient GET 它來渲染動態設定 UI，填好的值於 `SetParameters.ai_settings` 回傳。 |
| `POST /SetParameters` | recorder → wrapper | Per-channel 設定。JSON：`{version, mode, channel_id, analytics_event_api_url, image_width, image_height, jpg_compress, ai_settings, rois: [{sensitivity, threshold, rects: [{x,y}, ...]}]}`。v1.3 從 `ai_settings` 取 `jpg_compress`；v1.2 保留頂層舊欄位。 |
| `POST <analytics_event_api_url>` | wrapper → recorder | Detection callback（**僅 HTTP 結果模式**），body 含 keyframe 的 JPEG + ROI metadata + `items[]` 計數。ZMQ 結果模式下同一份 JSON 改走 ZMQ result plane。 |

### ZMQ frame plane（recorder → wrapper）

兩段式 ZMQ message；recorder 為 `PUSH(bind)`、wrapper 為 `PULL(connect)`：

- **part 0** — `ZmqFrameHeader`，packed **28 bytes**：`magic(u32) | version(u16) | codec(u8：1=H264, 2=H265) | is_keyframe(u8) | width(u16) | height(u16) | channel_id(u32) | timestamp(u64，Windows FILETIME 100ns UTC) | payload_sz(u32)`。
- **part 1** — 一張 coded picture（access unit）的原始已編碼 NAL bytes。

`channel_id`（== `exe_server_port + index`）把所有 channel 多工在同一組 socket 上；wrapper 依 `Port() == channel_id` 把每張影格路由到對應的 `ChannelPipeline`，再把 NAL 解成 I420。Magic `0x5A4D4600`、version `1`。

### ZMQ result plane（wrapper → recorder）

單段 ZMQ message，帶的是與 HTTP callback 相同的分析結果 JSON；wrapper 為 `PUSH(connect)`、recorder 為 `PULL(bind)`。由 `GenericAIDevice::parseGenericAIHttpAnalyticsEvent()` 解析。

### MMF frame plane（recorder → wrapper，舊版）

- Per-channel 命名：`ChannelFrame_<channelPort>`（第 `k` 路讀 `ChannelFrame_{port + k}`）。
- Layout 對齊舊版 `CSharp/SampleDLL/dllmain.cpp` 的 `MMF_Data`，讓 recorder 端 writer 不用改。
- `status` byte：`0=unused → 1=new frame → 2=consumed`。Wrapper poll 拿到後翻 `1 → 2`。
- `image_size` 可能大於緊湊排列的 I420 實際大小（recorder 解碼緩衝區以 32-byte 對齊的 plane stride 計算，例如 352x240 回報 130560 bytes，實際影像只有 126720 bytes）。Wrapper 以影格自帶的寬高算出 `width*height*3/2` 來驗證與複製，容忍多出來的餘量。

## Detector backend

預設 detector 於 `gai_config.h` 編譯期決定，並可用 `detector=` 於執行期覆寫：

```cpp
constexpr DetectorKind kDetectorKind = DetectorKind::Motion;   // 編譯期預設；detector= 可覆寫
constexpr bool         kPreferGpu    = true;                   // 僅 Person 有效
```

Process 內所有 channel 共用同一支 detector instance，透過 `SharedDetectorScheduler` 排程，結果由 `FrameDispatcher` 路由回原 channel。`kEnablePipelinedInference` 開啟（預設）且 detector 支援 pipelined inference 時（Person 支援、Motion 不支援），推論會拆成「CPU 預處理」與「GPU 推論 + 後處理」兩條 loop，中間夾一個小 queue，讓 CPU 跟 GPU 工作跨 frame 重疊。

### Person / object detection — YOLOX-M FP16 ONNX

- DirectML EP，失敗時自動 fallback 到 CPU。實際載入的 EP 可透過 `GAI_GetBackend` 讀回；timing/verbose 開關打開時，啟動也會在 console 印出（`EP=DirectML(0)` 或 `EP=CPU`）。
- Letterbox 預處理 → ONNX 推論 → score 過濾 + NMS → ROI overlap 過濾。
- Per-channel ROI 清單來自 `POST /SetParameters`；class 集合 / confidence / 物件大小範圍來自 `ai_settings`。

**參數對應（每次呼叫從 `/SetParameters` 拿）：**

| 欄位 | 範圍 | 效果 |
| --- | --- | --- |
| `threshold` | 0..100 | 線性對應到 YOLOX confidence 門檻 `0.20 .. 0.70`。越高越嚴（誤報少、漏報多）。 |
| `sensitivity` | — | 不使用。YOLOX 推論只有一個有效旋鈕（confidence score），兩個 slider 會壓到同一條軸。此欄位仍保留在 JSON 中以維持與 Motion 共用的協定格式。 |

Object detection 的 `ai_settings` 標量（經 `GAI_SetChannelAiSettings`）：`confidence`（0..1）、`classMask`（對固定 class table `person, car, bus, truck, motorcycle, bicycle, cat, dog` 的 bitmask）、以及 `min/maxObjectSize`（box 面積佔畫面 % 的範圍）。

Detector 內部常數（不在對外協定中）：NMS IoU 門檻固定 0.45；frame decimation `stride_n=1`（每幀都跑推論）。

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

`POST /SetParameters` 帶的是 per-ROI 的 `sensitivity[]` / `threshold[]` 陣列，但這組調參在語意上是**整個通道共用一組**：Native 端會挑第一個 polygon 有效（`rects.size() >= 3`）的 ROI slot，用它的 `(threshold, sensitivity)` 作為整個 frame 推論用的參數。如果整批 ROI 都不合法，保留上次的值；在第一次 `/SetParameters` 到達前，套用內建 schema 預設（Program.cs 於啟動時把 schema 預設種進每個 channel）。

### ROI 座標空間

`/SetParameters` 裡的 ROI 座標是以該 payload 的 `image_width × image_height` 為基準空間。影格（MMF 或由 ZMQ NAL 解出）帶有自己的解析度，兩者可能不同（例如同一支攝影機的子串流）。每次推論前 scheduler 會把 ROI 矩形——以及 Motion 回呼回聲的 polygon 點——等比換算到實際影格解析度，因此不論串流解析度為何，同一個 ROI 都對應到畫面上同一塊區域。回呼裡的座標一律是**實際影格的像素空間**，與隨附的 keyframe JPEG 對齊。

每通道的 frame pool 仍以第一次 `/SetParameters` 的 `image_width`/`image_height` 配置容量並在通道存活期間鎖定：**大於**該容量的影格會在進偵測前被丟棄（console 會輸出節流後的警告），之後再送不同解析度的 `/SetParameters` 也會被忽略並警告。要換成更大的解析度需重啟該通道。

## Native ABI

`GenericAI.Native.dll` 對外匯出一組小型 C ABI，由 `GenericAI.App` 透過 P/Invoke 取用（`Interop/NativeInterop.cs`）：

| Symbol | 用途 |
| --- | --- |
| `GAI_InitializeChannels(ports, count, detectorKind)` | 配置 per-channel pipeline、建立 detector（`0`=Motion、`1`=Person、`-1`=編譯預設）、啟動 scheduler。Degraded（模型載入失敗）init 回 `5`。 |
| `GAI_SetChannelParameters(port, *SettingParameters)` | 將最新的 `/SetParameters` payload（url、解析度、ROI、調參）推進對應 channel。 |
| `GAI_SetChannelAiSettings(port, confidence, classMask, sensitivity, threshold, minObjectSize, maxObjectSize)` | 推入 schema `ai_settings` 標量；detector 用不到的 key 傳 `<0`。 |
| `GAI_RegisterCallback(cb)` | 註冊 `FrameDispatcher` 觸發的偵測 callback。 |
| `GAI_RegisterLogCallback(cb)` | 註冊 host log sink；native 端 INFO / WARN / ERROR 訊息帶 `[native]` 前綴進 per-port log 檔。 |
| `GAI_GetBackend(buf, len)` | 讀回實際載入的 EP（`CPU` / `DirectML(0)`）。 |
| `GAI_GetInitError(buf, len)` | 讀 degraded-init 原因（模型載入失敗）以在 `/Alive` 回報。健康時回 `0`。 |
| `GAI_StartZmqReceiver(endpoint)` | 把 PULL frame socket connect 到 recorder BIND 的 PUSH；之後影格改走 ZMQ（NAL → 解碼）而非 MMF。於 init 後、第一次 `/SetParameters` 前呼叫。 |
| `GAI_StopZmqReceiver()` | 停 ZMQ frame receiver。 |
| `GAI_Deinitialize()` | 停 scheduler、釋放 detector、清 queue。 |

## 與 `CSharp/` 的關係

`CSharp/` 是舊版單通道 sample wrapper（`SampleWrapper.exe` + `SampleDLL.dll`），保留作為參考程式碼。GenericAI 用於正式部署、取代它：

| | `CSharp/SampleWrapper.exe` | `GenericAI.exe` |
| --- | --- | --- |
| 每 process channel 數 | 1 | N（單 process 多通道） |
| Detector | 只有 Motion | Motion 或 Person（YOLOX） |
| 傳輸 | MMF + HTTP | ZMQ（frame + result）或 MMF + HTTP |
| 產出 DLL | `SampleDLL.dll`（非 `GAI_*` exports） | `GenericAI.Native.dll`（`GAI_*` exports） |

Recorder 端透過 `AStreamPerimeter_GenericAI` 決定要 spawn 哪支 exe。兩支可同時放在硬碟上、不共用 DLL、不衝突。

## 專案結構

```
GenericAI_ZMQ/
  GenericAI.sln
  GenericAI.App/
    Program.cs              進入點 + cleanup 編排
    CommandLineArgs.cs      cmdline 解析（port/channel_count/mode/detector/endpoints/...）
    ChannelHandle.cs        per-channel 狀態（listener + MMF reader + queue）
    Protocol.cs             wire protocol 版本的單一來源
    SettingsSchema.cs       內建 /GetSettingsSchema payload（motion vs objectdetection）
    Http/
      HttpListenerHost.cs   /Alive、/GetLicense、/GetSettingsSchema、/SetParameters
      HttpPostClient.cs     HTTP 模式 detection callback 共用的 HttpClient
      HttpEnvelope.cs       結果 callback JSON payload（version + keyframe + items + rois）
      ParameterStore.cs     來自 /SetParameters 的 process-wide url + jpgQuality 快取
    Pipeline/
      FrameDispatcher.cs    detector callback -> channel router
      EncodeWorker.cs       JPEG encode worker（turbojpeg）
      SendWorker.cs         結果送出 worker（HTTP POST 或 ZMQ PUSH）
      ZmqResultSender.cs    ZMQ PUSH 結果 sink（connect 到 module BIND 的 PULL）
      RoundRobinTaker.cs    worker pool 共用的公平多佇列取出器
      DropCounter.cs        各 pipeline 階段的 drop 計數
    Interop/
      NativeInterop.cs      GAI_* exports 的 P/Invoke 表面
      TurboJpegInterop.cs   turbojpeg 的 P/Invoke 表面
      DetectorType.cs       對應 native DetectorKind 的 managed 列舉
    Diagnostics/
      FileLogger.cs         非同步檔案 logger（%ProgramData%\Spark\GenericAI\Logs）
      ConsoleLog.cs         console 狀態輸出開關
      HealthState.cs        於 /Alive 呈現的 process 健康狀態
      TimingRecorder.cs     可選的 timing instrumentation
  GenericAI.Native/         磁碟上維持扁平；VS 內以 .vcxproj.filters 分組
    exports.cpp             GAI_* C ABI
    host_log.{h,cpp}        native -> host log 橋接（GAI_RegisterLogCallback）
    shared_detector_scheduler.{h,cpp}   單 detector + N-channel 排程
    channel_pipeline.{h,cpp}            per-channel work queue
    param_snapshot.{h,cpp}              per-channel /SetParameters 儲存（ROI + 調校參數）
    detector_factory.{h,cpp}            挑 Motion 或 Person（編譯預設 + detectorKind）
    detector_motion.{h,cpp}             frame-diff detector
    detector_person.{h,cpp}             YOLOX-M FP16 detector（DirectML / CPU）
    mmf_reader.{h,cpp}                  per-channel MMF poll loop
    zmq_frame_receiver.{h,cpp}          ZMQ PULL frame receiver
    zmq_frame_header.h                  28-byte frame header（對齊 recorder 的 ZmqFrameProtocol.h）
    nal_decoder.{h,cpp}                 H.264/H.265 NAL -> I420（FFmpeg）
    buffer_pool.{h,cpp}                 pooled frame buffer
    class_table.h                       固定的可偵測 class 順序
    detector 幾何：roi_geometry.h、yolox_geometry.h
    gai_abi.h / gai_config.h            ABI 宣告 + 編譯期 flag
  native-deps/
    models/yolox_m_fp16.onnx + LICENSE.md + VERSION.txt
    win-x64/turbojpeg.dll + LICENSE.md + VERSION.txt
```
