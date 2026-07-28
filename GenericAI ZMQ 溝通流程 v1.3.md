# GenericAI × ARGO Recorder 溝通流程 — ZMQ 模式（v1.3）

本文只講**一件事**：ZMQ 模式下，`GenericAI.exe` 如何啟動，之後與 ARGO Recorder 之間**誰下了什麼指令、對方做什麼動作**。

**ZMQ 模式特性**

- **可本機或遠端**：影格與結果走 ZMQ TCP socket，不需同機。
- **Recorder 不 spawn wrapper**：wrapper 由人手動（或在遠端）啟動，**主動連入** Recorder 綁好的 socket。
- **控制走 HTTP、影格走 ZMQ、結果走 ZMQ**（都不用 HTTP POST 結果）。
- **socket 方向**：Recorder 負責 **bind**、wrapper 負責 **connect**；因此 wrapper 可獨立（重）啟動，ZMQ 自動重連。

> 對照組：MMF 模式（本機、Recorder 主動 spawn）請見另一份文件。

---

## 1. 啟動

### 1a. Recorder 端（先把 socket 綁好）

Recorder 的裝置檢查迴圈在偵測到主機可連時：

- **bind** frame `PUSH` 於 `tcp://*:<ai_stream_port>`（送影格用）
- **bind** result `PULL` 於 `tcp://*:<http_server_port>`（收結果用）
- 開始週期性 `GET /Alive` 探測 wrapper 的控制埠

> 這兩個埠與 HTTP 控制埠（`exe_server_port`）是**三個不同的埠**。result 埠沿用 `http_server_port` 這個數值，但綁的是 ZMQ PULL，不是 HTTP server。

### 1b. Wrapper 端（手動啟動並連入）

以 Recorder 的實際埠啟動（範例：Recorder 同機、8 channel）：

```text
GenericAI.exe port=51000 channel_count=8 mode=single detector=motion ^
              server_ip=127.0.0.1 stream_port=61000 result_port=9903
```

- `server_ip + stream_port + result_port` 會展開成
  `frame_endpoint=tcp://127.0.0.1:61000`、`result_endpoint=tcp://127.0.0.1:9903`。
- `port=51000` 是 HTTP 控制埠（channel 0）；其餘 channel `51001…51007`。
- **ZMQ 必須 `mode=single`**（一個程序服務全部 channel，用 `channel_id` 分流）。

啟動後 wrapper 自己做的事：

1. 在 `51000…51007` 每個 channel 各開一個 HTTP listener。
2. **connect** frame `PULL` → `frame_endpoint`（61000）。
3. **connect** result `PUSH` → `result_endpoint`（9903）。

> `port=` 要等於 Recorder 的 `exe_server_port`、`stream_port` 要等於 `ai_stream_port`、`result_port` 要等於 `http_server_port`。填錯 `result_port`（例如填成 stream 埠）會變成「有幀進、卻收不到結果」。

---

## 2. 來回溝通流程

以下為時間順序。Recorder 已在步驟 0 綁好 socket（§1a）。

| # | 發起方 | 指令 / 訊息 | 對方收到後的動作 |
| --- | --- | --- | --- |
| 0 | Recorder（自身） | bind frame PUSH + result PULL | 開好通道，等 wrapper 連入。 |
| 1 | Recorder → wrapper | `GET /Alive` | wrapper 回 `200 {"status":"ok","version":"1.3"}`。Recorder 據此把裝置標記為**已連線**並得知版本。 |
| 2 | Recorder → wrapper | `GET /GetSettingsSchema` | wrapper 回設定 schema JSON。Recorder 快取給 ConfigClient 渲染 AI 設定 UI（每次連線抓一次）。 |
| 3 | Recorder → wrapper | `POST /SetParameters`（每個 channel 一次） | wrapper 解析解析度、ROI、`ai_settings`，套用到該 channel。回 `200`。（ZMQ 模式不需建立共享記憶體。） |
| 4 | Recorder → wrapper | 送影格（ZMQ 影格平面） | Recorder 把**編碼後的 H.264/H.265** access unit 以 2-part ZMQ 訊息 PUSH 出去（part 0 = `ZmqFrameHeader` 含 `channel_id`，part 1 = NAL bytes）。 |
| 5 | wrapper（內部） | 收影格 + 解碼 | wrapper 從 frame PULL 收到，依 `channel_id` 路由到對應 channel，NAL 解碼成 I420，送進偵測。 |
| 6 | wrapper → Recorder | 送結果（ZMQ 結果平面） | **有偵測到時才送**。單一 part ZMQ 訊息，內容為結果 JSON（`port_num`、`keyframe`、`timestamp`、`rois_rects`、`items`）。Recorder 的 result PULL 執行緒收下，依 `port_num` 對應到 channel，推成事件顯示在 Client。 |

指令 1–3 是「連上線」的一次性交握；4–6 是穩態迴圈。

---

## 3. 穩態迴圈（連上線後持續進行）

- **影格**：Recorder 持續 PUSH 編碼影格（步驟 4）→ wrapper 解碼＋偵測（步驟 5）→ 有結果就 PUSH 回去（步驟 6）。
- **健康檢查**：Recorder 週期性 `GET /Alive`。連續無回應 → 判定斷線 → 該裝置標記為 disconnect，並持續嘗試重連（socket 保持綁著，wrapper 一回來就接上）。
- **改設定**：使用者在 ConfigClient 改了 ROI／AI 設定 → Recorder 對該 channel 重送 `POST /SetParameters`（步驟 3）。
- **授權**（選用）：Recorder 可 `GET /GetLicense` 查授權。

---

## 4. 關閉

- wrapper 關閉 → Recorder 的 `/Alive` 無回應 → 裝置標記 disconnect，socket 仍綁著等待重連。
- Recorder 停用裝置 → 解除 socket bind（`PUSH`/`PULL` 關閉）。

---

## 5. 指令 → 動作 速查

| 指令 / 訊息（方向） | 觸發的動作 |
| --- | --- |
| bind PUSH/PULL（R 自身） | 綁好影格／結果通道，等 wrapper 連入 |
| `GET /Alive`（R→W） | 回健康狀態＋版本；Recorder 判斷連線 |
| `GET /GetSettingsSchema`（R→W） | 回設定 schema；Recorder 快取給 UI |
| `POST /SetParameters`（R→W） | 套用該 channel 設定（ROI／AI 設定） |
| 影格訊息 PUSH（R→W） | 2-part：header(含 `channel_id`) + NAL；wrapper 依 `channel_id` 分流、解碼、偵測 |
| 結果訊息 PUSH（W→R） | 送出偵測結果；Recorder 依 `port_num` 路由 |
| `GET /GetLicense`（R→W） | 查授權 |

R = Recorder、W = wrapper（`GenericAI.exe`）。

---

## 附：埠對應與影格標頭

**三個埠（都不同）**

| 用途 | wrapper 參數 | = Recorder 的 |
| --- | --- | --- |
| HTTP 控制（/Alive、/SetParameters） | `port` | `exe_server_port` |
| 影格平面（PULL 連入） | `stream_port` / `frame_endpoint` | `ai_stream_port` |
| 結果平面（PUSH 連入） | `result_port` / `result_endpoint` | `http_server_port` |

**影格訊息 part 0 標頭**

```cpp
#pragma pack(push, 1)
struct ZmqFrameHeader {
    uint32_t magic;        // 0x5A4D4600
    uint16_t version;      // 1
    uint8_t  codec;        // 1 = H264, 2 = H265
    uint8_t  is_keyframe;  // 1 = keyframe, 0 = P/B
    uint16_t width;        // coded width（僅供參考）
    uint16_t height;       // coded height（僅供參考）
    uint32_t channel_id;   // 路由到 channel（exe_server_port + index）
    uint64_t timestamp;    // Windows FILETIME（100ns ticks，UTC）
    uint32_t payload_sz;   // part 1 的 NAL byte 數
};
#pragma pack(pop)
```

- part 1 = 一張編碼影像的 raw NAL bytes（`payload_sz` bytes）；keyframe 應可自解（SPS/PPS 內含或前置）。
- SetParameters JSON 與結果 JSON 的完整欄位，見《ARGO 通用 AI 整合指南 v1.3》§3–§4。
