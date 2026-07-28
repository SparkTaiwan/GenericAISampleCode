# ARGO 通用 AI 整合指南 — 1.3 版

本文說明外部 AI／分析 wrapper 如何與 **Spark Recorder / ARGO** 整合，是 1.2 版範例程式指南的後續版本，內容對應目前的 `GenericAI_ZMQ` wrapper。

**相較 1.2 的變更**

- 新增 **ZMQ 傳輸**，作為正式環境的預設路徑（可本機或遠端）。舊的 Shared Memory + HTTP 路徑仍保留，供本機、向後相容的部署使用。
- **單一程序服務 N 個 channel**（single 模式），以 `channel_id` 多工，取代舊的「一 channel 一程序」。
- **自描述設定 schema**（`GET /GetSettingsSchema`），每個欄位帶 **scope**（`device` / `channel` / `roi`），並在 Motion（sensitivity / threshold）之外新增 **物件偵測** 欄位（confidence / classes / object size）。
- **更豐富的結果事件**：除了 keyframe 與 ROI 命中外，還帶各類別計數（`items`）。
- **健康狀態回報**：`/Alive` 回 `{"status","version"}`，讓 degraded 的 wrapper（例如模型缺失）能被辨識，而不是被無限重啟。
- 參考 wrapper 為 `GenericAI.exe`（即 1.2 的 `SampleWrapper.exe`）。

> 線上的 version 字串（`"version": "1.3"`）決定協定層級。1.3 的 wrapper 仍接受 1.2 的 payload。

---

## 1. 架構與資料流

Recorder（ARGO 端的 `AStreamPerimeter_GenericAI` AI-service 模組）與 wrapper（`GenericAI.exe`）之間由**三個獨立的平面**連接：

```text
Spark Recorder / AStreamPerimeter_GenericAI         GenericAI.exe (wrapper)
+-------------------------------+                   +-------------------------------+
| HTTP client  (control plane)  | ---(1) request--> | HTTP listeners (per channel)  |
|  GET  /Alive                  |                   |  /Alive  (health + version)   |
|  GET  /GetSettingsSchema      | <--(1a) reply---- |  /GetSettingsSchema           |
|  POST /SetParameters          |                   |  /SetParameters               |
|  GET  /GetLicense             |                   |  /GetLicense                  |
+-------------------------------+                   +-------------------------------+

+-------------------------------+                   +-------------------------------+
| Frame plane (source)          | ==(2) frames====> | Frame plane (sink)            |
|  ZMQ PUSH  (encoded H.264/5)  |                   |  ZMQ PULL -> NAL decode       |
|   -- or legacy --             |                   |   -- or legacy --             |
|  MMF writer (raw I420)        |                   |  MMF reader                   |
+-------------------------------+                   +-------------------------------+

+-------------------------------+                   +-------------------------------+
| Result plane (sink)           | <=(3) results==== | Result plane (source)         |
|  ZMQ PULL  (analytics JSON)   |                   |  ZMQ PUSH                     |
|   -- or legacy --             |                   |   -- or legacy --             |
|  HTTP /PostAnalyticsResult    |                   |  HTTP POST                    |
+-------------------------------+                   +-------------------------------+
```

1. **控制平面（HTTP/JSON）。** Recorder 用 `/Alive` 探測、用 `/GetSettingsSchema` 取得設定 UI schema、用 `POST /SetParameters` 下發每個 channel 的設定。控制用的 listener 一律在 wrapper 的 HTTP 控制埠（`port=`）上，與 frame/result 傳輸方式無關。
2. **影格平面（frame plane）。** ZMQ（wrapper 端解碼 H.264/H.265，可遠端）或舊的 MMF 共享記憶體（raw I420，僅限本機）。
3. **結果平面（result plane）。** ZMQ（分析結果 JSON 由 wrapper PUSH 回去）或舊的 HTTP POST 到 `analytics_event_api_url`。

**元件**

| 層 | 實作 | 產出 | 職責 |
| --- | --- | --- | --- |
| Host | C#（.NET Framework 4.8） | `GenericAI.exe` | HTTP 控制 listener、ZMQ 結果傳送／MMF 讀取、JPEG 編碼與傳送、native interop。 |
| Detector | C++（MSVC v140） | `GenericAI.Native.dll` | 偵測器（Motion、YOLOX person）、per-channel pipeline、ZMQ frame receiver + NAL 解碼。匯出 `GAI_*` C ABI。 |

---

## 2. AI 整合規格

### 2.1 通訊 API

所有控制端點都是 wrapper 控制埠上的 HTTP。`{port}` 為 wrapper 的 `exe_server_port`（channel 0）；其餘每個 channel 監聽 `exe_server_port + index`。

| # | 方向 | Method + URL | Body | 用途 |
| --- | --- | --- | --- | --- |
| 2.1.1 | Recorder → wrapper | `POST http://{host}:{port}/SetParameters` | JSON（見 §3） | 設定一個 channel（解析度、ROI、AI 設定）。回 `200`。 |
| 2.1.2 | Recorder → wrapper | `GET http://{host}:{port}/Alive` | — | 健康狀態 + 協定版本。回 `200`，body `{"status":"ok","version":"1.3"}`（degraded 時為 `"status":"error"` + `"message"`）。 |
| 2.1.3 | Recorder → wrapper | `GET http://{host}:{port}/GetSettingsSchema` | — | 回傳設定 schema JSON（§3.2），ConfigClient 用它渲染 AI 設定 UI。 |
| 2.1.4 | Recorder → wrapper | `GET http://{host}:{port}/GetLicense` | — | 授權驗證／請求。回 `200`。 |
| 2.1.5 | wrapper → Recorder | 結果事件 | JSON（§4.5） | 分析結果。**ZMQ**：由 result plane PUSH（預設）。**Legacy**：`POST {analytics_event_api_url}`（`/PostAnalyticsResult`）。 |

> 本機 wrapper 的 `host` 為 `127.0.0.1`。遠端 wrapper 的控制 listener 需綁定可連的介面（`http_host=`），recorder 則連到該 IP。

### 2.2 傳輸方式選擇

wrapper **純粹由啟動參數**（§7）決定各平面的傳輸方式：

- 有帶 `frame_endpoint` → 影格走 **ZMQ**；未帶 → 走 **MMF** 共享記憶體。
- 有帶 `result_endpoint` → 結果走 **ZMQ**；未帶 → 走 **HTTP** `POST /PostAnalyticsResult`。

兩個平面各自獨立。正式環境兩者都用 ZMQ；舊的本機路徑用 MMF + HTTP。

---

## 3. 建議的詳細設定

### 3.1 `SetParameters` 請求（v1.3）

最多 **10 個 ROI**，每個 ROI 最多 **10 個點**。物件偵測調校可**依 ROI**（`rois[i].ai_settings`）或**依 channel**（top-level `ai_settings`）帶；ROI 的值會覆蓋 channel 的值。

```json
{
  "version": "1.3",
  "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
  "image_width": 1280,
  "image_height": 720,
  "draw_roi": true,
  "ai_settings": {
    "jpg_compress": 30,
    "trigger_interval": 1,
    "confidence": 0.70,
    "classes": ["person", "car"],
    "object_size_min": 0.0,
    "object_size_max": 100.0
  },
  "rois": [
    {
      "sensitivity": 50,
      "threshold": 50,
      "ai_settings": {
        "confidence": 0.80,
        "classes": ["person"],
        "object_size_min": 0.5,
        "object_size_max": 60.0
      },
      "rects": [
        {"x": 100, "y": 100},
        {"x": 200, "y": 100},
        {"x": 200, "y": 200},
        {"x": 100, "y": 200}
      ]
    }
  ]
}
```

注意：

- `analytics_event_api_url` 只在**舊的 HTTP** 結果路徑使用；ZMQ 結果模式會忽略它。
- v1.3 的 `jpg_compress` 與 `trigger_interval` 來自 `ai_settings`（channel scope）。top-level 的 `jpg_compress` 仍接受，供 v1.2 相容。
- Motion 用 `sensitivity` / `threshold`（依 ROI）。物件偵測用 `confidence` / `classes` / `object_size_*`。偵測器會忽略它用不到的欄位。

### 3.2 `GetSettingsSchema` 回應

`GET /GetSettingsSchema` 回傳 ConfigClient 用來渲染的欄位 schema；使用者填的值會原樣回到 `SetParameters`（channel 欄位在 top-level `ai_settings`，ROI 欄位在 `rois[i].ai_settings`）。每個欄位帶 **scope**（§4.2）。以下為 Motion 範例：

```json
{
  "schema_version": "1.0",
  "default_locale": "zh-TW",
  "fields": [
    { "key": "jpg_compress", "type": "int", "scope": "channel",
      "label": {"zh-TW": "JPEG 壓縮品質", "en": "JPEG Quality"},
      "default": 30, "value": 30, "min": 1, "max": 100 },
    { "key": "sensitivity", "type": "int", "scope": "roi",
      "label": {"zh-TW": "靈敏度", "en": "Sensitivity"},
      "default": 50, "value": 50, "min": 1, "max": 100 },
    { "key": "threshold", "type": "int", "scope": "roi",
      "label": {"zh-TW": "門檻值", "en": "Threshold"},
      "default": 25, "value": 25, "min": 1, "max": 100 },
    { "key": "trigger_interval", "type": "int", "scope": "channel",
      "label": {"zh-TW": "觸發間隔（秒）", "en": "Trigger Interval (sec)"},
      "default": 1, "value": 1, "min": 0, "max": 3600 }
  ]
}
```

物件偵測 schema 會把 `sensitivity`/`threshold` 換成 `confidence`（float，`roi`）、`classes`（string_array，`roi`；可計數的類別集合）、`object_size_min` / `object_size_max`（float，畫面佔比 %，`roi`）。`label` / `description` / `option.label` 一律是 locale map（`{ "zh-TW": …, "en": … }`）。

### 3.3 Schema 欄位定義

`fields[]` 的每一項是一個欄位物件。**ConfigClient 會依欄位的 `type` 渲染成*不同的 UI 元件***——這是你設計自己的 schema 時最重要的一點，因為 `type` 決定了操作人員實際看到與編輯的介面。

**欄位屬性**

| 屬性 | 必要 | 適用於 | 說明 |
| --- | --- | --- | --- |
| `key` | ✔ | 全部 | 欄位鍵。回填為 `ai_settings` 的 key（AI 只讀 `key` + `value`，不儲存 schema metadata）。 |
| `type` | ✔ | 全部 | 決定 UI 元件——見下表。未指定 → `string`。 |
| `scope` | | 全部 | `device` / `channel` / `roi`（§4.2）。未指定 → `channel`。 |
| `label` | ✔ | 全部 | 顯示名稱。純字串或 locale map。 |
| `description` | | 全部 | 顯示為控制項的 tooltip。 |
| `unit` | | 數值類 | 單位字串，唯讀檢視時附加在值之後。 |
| `required` | | 全部 | 是否必填。 |
| `default` / `value` | ✔ | 全部 | 內建預設值 / 現值（serve 時 `value == default`；recorder 回填設定時覆寫 `value`）。 |
| `min` / `max` / `step` | | 數值類 | Slider 的範圍與刻度粒度。 |
| `options` | ✔ | `enum`、`string_array` | 選項清單，每項 `{ "value": …, "label": … }`（label 可為 locale map）。 |
| `counting` | | `string_array` | 標示此欄位的選項為可計數項目（各項計數會出現在結果 `items`，§4.5）。 |

**`type` → UI 元件**

| `type` | 在 ARGO Config 的 UI 元件 | 線上 value 型別 | 說明 |
| --- | --- | --- | --- |
| `int` / `float` | **Slider（滑桿）** | number | 以 `min`/`max` 為範圍，`step` 為刻度。`int` 吸附到 1，`float` 吸附到 `step`。 |
| `int_range` / `float_range` | **兩個輸入框（low / high）** | `[low, high]` | 範圍（兩個值）。 |
| `bool` | **核取方塊（開／關）** | boolean | |
| `string` | **文字框** | string | 自由文字。 |
| `enum` | **下拉選單（單選）** | string | 從 `options` 選出剛好一個 `value`。 |
| `string_array` | **核取清單（多選）** | string[] | `options` 的任意子集。搭配 `counting` 時，這些選項就是可計數集合（例如偵測類別）。 |

**value 回填。** 操作人員編輯後，ConfigClient 會把每個欄位輸出成扁平的 `{ key: value }`（不含 schema metadata）——channel scope 的欄位放進 top-level `ai_settings`，roi scope 的欄位放進 `rois[i].ai_settings`。value 型別依上表。

**實際對應（現行內建 schema）：** `sensitivity` / `threshold` / `jpg_compress` / `trigger_interval`（`int`）→ Slider；`confidence` / `object_size_min` / `object_size_max`（`float`）→ Slider；`classes`（`string_array` + `options` + `counting`）→ 核取清單。

---

## 4. 參數說明

### 4.1 Top-level 參數（`SetParameters`）

| 名稱 | 型別 | 範例 | 說明 |
| --- | --- | --- | --- |
| `version` | string | `"1.3"` | 協定版本。決定走 1.3 或 1.2 行為。 |
| `analytics_event_api_url` | string (URL) | `http://127.0.0.1:9901/PostAnalyticsResult` | 結果的 HTTP 端點（僅舊的結果路徑使用）。 |
| `image_width` / `image_height` | int | `1280` / `720` | 影格解析度（像素）。 |
| `draw_roi` | bool | `true` | 是否在回傳的 keyframe JPEG 上畫出 ROI 邊框。 |
| `jpg_compress` | int (1–100) | `30` | keyframe JPEG 品質（越高越好、檔越大）。v1.3 從 `ai_settings` 取；top-level 供 v1.2 相容。 |
| `ai_settings` | object | 見 §3.1 | Channel scope 的設定（schema 中 `scope:"channel"` 的欄位，各帶其 `value`）。 |
| `rois` | array | 見 §4.3 | ROI 清單（最多 10 個）。 |

### 4.2 Scope

每個 schema 欄位都宣告它在哪裡編輯、在線上如何流動：

| scope | 一個值對應… | 編輯位置 | 攜帶於 |
| --- | --- | --- | --- |
| `device` | 整台裝置 | 裝置頁 | top-level `ai_settings`（裝置層級） |
| `channel` | 一路智慧串流 | 串流頁 | `SetParameters.ai_settings`（top-level） |
| `roi` | 一個偵測區域 | ROI 編輯器 | `SetParameters.rois[i].ai_settings` |

原則：「什麼算命中」（偵測調校）→ `roi`；「如何輸出／整條串流」→ `channel`；「整台裝置共用一個設定」→ `device`。

### 4.3 `rois` 陣列

每個 ROI 帶偵測參數與其多邊形點。

| 名稱 | 型別 | 範例 | 說明 |
| --- | --- | --- | --- |
| `sensitivity` | int (0–100) | `50` | **Motion**：偵測靈敏度（越高越敏感）。 |
| `threshold` | int (0–100) | `50` | **Motion**：變化門檻（越低越容易觸發）。 |
| `ai_settings` | object | 見 §3.1 | **物件偵測** 的 per-ROI 調校：`confidence`（0–1）、`classes`（支援類別的子集）、`object_size_min`/`object_size_max`（畫面佔比 %，0–100）。未帶 → 沿用 channel 的值。 |
| `rects` | array | 4 點 | ROI 多邊形（§4.4）。 |

支援的偵測類別（固定順序；`classes` 的值與結果 `items` 的 name 都用這些）：`person`、`car`、`bus`、`truck`、`motorcycle`、`bicycle`、`cat`、`dog`。

### 4.4 `rects` 參數

由最多 10 個 `{x, y}` 點組成的多邊形；範例與 recorder 使用 4 個角點。建議固定順序：

**左上 → 右上 → 右下 → 左下**

```json
"rects": [
  {"x": 100, "y": 100},
  {"x": 200, "y": 100},
  {"x": 200, "y": 200},
  {"x": 100, "y": 200}
]
```

`x` / `y` 為整數像素座標，自影像左上角起算。順序不一致可能產生形狀錯誤的多邊形。

### 4.5 辨識事件結果

每當一個 channel 產生偵測時，透過 ZMQ result plane（或舊模式的 HTTP POST）送出。

```json
{
  "version": "1.3",
  "port_num": 51000,
  "keyframe": "/9j/4AAQSkZJR...",
  "timestamp": 15003215760000,
  "rois_rects": [
    [ {"x":0,"y":0}, {"x":10,"y":0}, {"x":10,"y":10}, {"x":0,"y":10} ]
  ],
  "items": [
    { "name": "person", "value": "3" },
    { "name": "car",    "value": "1" }
  ]
}
```

| 名稱 | 型別 | 說明 |
| --- | --- | --- |
| `version` | string | 結果格式版本。 |
| `port_num` | int | Channel 識別碼 = 該 wrapper channel 的埠（`exe_server_port + index`）。recorder 依此路由結果。 |
| `keyframe` | string (Base64) | JPEG keyframe，Base64 編碼。可依 `draw_roi` 畫上 ROI 邊框。 |
| `timestamp` | int | 影格時間，Windows FILETIME（100ns ticks，UTC）。 |
| `rois_rects` | array of arrays | 觸發的 ROI 多邊形，每個為 `{x,y}` 點的陣列。 |
| `items` | array | 各類別偵測計數，格式 `{ "name": "<class>", "value": "<count>" }`。`name` 對應某個 `classes` 選項。空時省略。 |

---

## 5. 共享記憶體設定（舊路徑／本機）

當**未帶** `frame_endpoint` 時使用。recorder 把 raw I420 影格寫入每個 channel 的具名共享記憶體區段，wrapper 讀取。Windows 共享記憶體，最高解析度 1920×1080。

```cpp
constexpr std::int64_t kMmfHeader = 0x1234;
constexpr std::int64_t kMmfFooter = 0x4321;
constexpr std::size_t  kMmfFrameCapacity = 1920ull * 1080ull * 3ull;

struct MmfData {
    std::int64_t  header;        // kMmfHeader
    int           image_status;  // 0 = 閒置, 1 = 新影格, 2 = 已取用（detection got frame）
    int           image_width;
    int           image_height;
    int           image_size;    // 影格 byte 數
    std::uint64_t timestamp;     // Windows FILETIME（100ns ticks，UTC）
    unsigned char image_data[kMmfFrameCapacity];  // raw I420
    std::int64_t  footer;        // kMmfFooter
};
```

- **區段名稱：** `ChannelFrame_<port>`，`<port>` 為該 channel 的控制埠（`exe_server_port + index`）。
- **`image_status`：** `0` = 無資料，`1` = 新影格待 wrapper 讀取，`2` = wrapper 已取用。
- wrapper 會驗證 `header`/`footer`、等待 `image_status == 1`、讀取 I420 payload（大小由 `image_width`/`image_height` 推得）、執行偵測，並將影格標記為已取用。

---

## 6. ZMQ 設定

正式環境的預設傳輸。兩個平面都是 ZMQ TCP socket。**recorder 負責 bind、wrapper 負責 connect**，所以 wrapper 可獨立（重）啟動，ZMQ 會自動重連。

| 平面 | Recorder 端（bind） | Wrapper 端（connect） | Payload |
| --- | --- | --- | --- |
| Frame | `PUSH` 於 `tcp://*:<ai_stream_port>` | `PULL` ← `frame_endpoint` | 編碼後的 H.264/H.265 access unit |
| Result | `PULL` 於 `tcp://*:<http_server_port>` | `PUSH` → `result_endpoint` | 分析結果 JSON（§4.5） |

> Result plane 的埠**沿用** recorder 設定裡的 **`http_server_port`** 數值；ZMQ 模式下在該埠綁的是 ZMQ `PULL` socket，**不是** HTTP server。frame 與 result 埠彼此獨立（`ai_stream_port` ≠ `http_server_port`），兩者也都與 HTTP 控制埠（`exe_server_port`）不同。

**影格訊息格式。** 每個影格是 **2-part** ZMQ 訊息；`channel_id` 讓多個 channel 共用同一 socket（receiver 依 `channel_id = exe_server_port + index` 路由到對應 channel）：

- **Part 0** — `ZmqFrameHeader`（packed）：

```cpp
#pragma pack(push, 1)
struct ZmqFrameHeader {
    uint32_t magic;        // 0x5A4D4600  ('Z','M','F',0)
    uint16_t version;      // 1
    uint8_t  codec;        // 1 = H264, 2 = H265
    uint8_t  is_keyframe;  // 1 = IDR/keyframe, 0 = P/B
    uint16_t width;        // coded width（僅供參考）
    uint16_t height;       // coded height（僅供參考）
    uint32_t channel_id;   // 路由到 channel（exe_server_port + index）
    uint64_t timestamp;    // Windows FILETIME（100ns ticks，UTC）
    uint32_t payload_sz;   // part 1 的 NAL byte 數
};
#pragma pack(pop)
```

- **Part 1** — 一張編碼影像（access unit）的 raw NAL bytes（`payload_sz` bytes）。keyframe 應可自解（SPS/PPS 內含或前置）。

**結果訊息格式。** 單一 part 的 ZMQ 訊息，內容即 §4.5 的結果 JSON（`port_num` 欄位標明 channel）。

**Socket 調校**（參考）：frame `PUSH` 用有限的送出逾時 + 有界 HWM，讓 wrapper 卡住時丟幀而非塞住編碼器；result `PULL` 用短的接收逾時，讓迴圈能觀察關機。關閉時 `LINGER=0`。

---

## 7. 執行與命名慣例

### 7.1 啟動參數

```text
GenericAI.exe port=<int> [channel_count=<N>] [mode=single|multi] [detector=motion|objectdetection]
              [server_ip=<ip> result_port=<port> stream_port=<port>]
              [frame_endpoint=tcp://host:port] [result_endpoint=tcp://host:port]
              [http_host=<ip|+>] [encode_workers=<N>] [send_workers=<M>]
```

| 參數 | 對應（recorder） | 說明 |
| --- | --- | --- |
| `port` | `exe_server_port` | HTTP 控制埠（`/Alive`、`/SetParameters`）與 channel-0 的 id。**正式環境必填。** |
| `channel_count` | channel 數 | 此程序服務的 channel 數（`single` 模式）。控制 listener 開在 `port … port+N-1`。 |
| `mode` | `process_mode` | `single` = 一程序服務全部 channel（**ZMQ 必須**）；`multi` = 一 channel 一程序。 |
| `detector` | 偵測型別 | `motion` 或 `objectdetection`。未帶 → wrapper 的編譯期預設。 |
| `frame_endpoint` | `tcp://<ip>:<ai_stream_port>` | 要 PULL 的 ZMQ 影格平面。未帶 → MMF 影格。 |
| `result_endpoint` | `tcp://<ip>:<http_server_port>` | 要 PUSH 的 ZMQ 結果平面。未帶 → HTTP 結果 POST。 |
| `server_ip` + `result_port` + `stream_port` | recorder IP + `http_server_port` + `ai_stream_port` | 簡寫，展開成 `result_endpoint`/`frame_endpoint`。須一起給。 |
| `http_host` | — | 控制 listener 的綁定位址（預設 `127.0.0.1`；遠端 recorder 用本機 IP 或 `+`）。 |

**ZMQ 啟動範例**（一程序、8 channel、recorder 同機）：

```text
GenericAI.exe port=51000 channel_count=8 mode=single detector=motion ^
              server_ip=127.0.0.1 stream_port=61000 result_port=9903
```

**MMF 啟動範例**（舊路徑，一 channel 一程序）：

```text
GenericAI.exe port=51007 mode=multi detector=motion
```

### 7.2 命名與規則

1. 執行檔可改名；請設定 ARGO 呼叫正確檔名。**參數格式固定不變。**
2. `port=` 必填且等於 `exe_server_port`；channel id 為 `exe_server_port + index`。
3. ZMQ 需 `mode=single`；`mode=multi` 時 recorder 不會啟用 ZMQ。
4. 三個埠彼此不同：控制（`exe_server_port`）、影格（`ai_stream_port`）、結果（`http_server_port`）。把 `result_port` 填成 stream 埠是常見錯誤——會變成有幀進、卻收不到結果。
5. 執行檔應為**已簽章的應用程式**，避免被 Windows／防毒攔阻。

---

## 8. 授權

Generic AI Detection 授權序號與 channel 數由 **台灣迪維科股份有限公司（Taiwan Spark Technology Co., Ltd.）** 提供。若需啟用上述功能，請聯繫您的窗口。
