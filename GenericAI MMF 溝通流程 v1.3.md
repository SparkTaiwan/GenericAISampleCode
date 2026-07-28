# GenericAI × ARGO Recorder 溝通流程 — MMF 模式（v1.3）

本文只講**一件事**：MMF 模式下，`GenericAI.exe` 如何啟動，之後與 ARGO Recorder 之間**誰下了什麼指令、對方做什麼動作**。

**MMF 模式特性**

- **本機限定**：影格走 Windows 共享記憶體，兩邊必須同一台機器。
- **Recorder 主動啟動 wrapper**：Recorder 會自己 spawn `GenericAI.exe`（crash 也會自動重啟）。
- **控制走 HTTP、影格走共享記憶體、結果走 HTTP POST**。

> 對照組：ZMQ 模式（可遠端、wrapper 自行啟動並連入）請見另一份文件。

---

## 1. 啟動

Recorder（single 模式，一個程序服務所有 channel）用類似指令 spawn wrapper：

```text
GenericAI.exe port=51000 channel_count=8 mode=single detector=motion
```

- 沒有 `frame_endpoint` / `result_endpoint` → 自動走 MMF + HTTP。
- `port=51000` 是 HTTP 控制埠（channel 0）；其餘 channel 為 `51001…51007`。

啟動後 wrapper 自己做的事（還沒跟 Recorder 溝通）：

1. 在 `51000…51007` 每個 channel 各開一個 HTTP listener。
2. 等待 `/SetParameters`（此時還沒開共享記憶體）。

---

## 2. 來回溝通流程

以下為時間順序。「發起方 → 對方」表示誰主動送、對方收到後做什麼。

| # | 發起方 | 指令 / 訊息 | 對方收到後的動作 |
| --- | --- | --- | --- |
| 1 | Recorder → wrapper | `GET /Alive` | wrapper 回 `200 {"status":"ok","version":"1.3"}`。Recorder 藉此確認 wrapper 活著並得知協定版本。degraded 時回 `"status":"error"`＋`message`，Recorder 顯示原因而不重啟。 |
| 2 | Recorder → wrapper | `GET /GetSettingsSchema` | wrapper 回傳設定 schema JSON。Recorder 快取給 ConfigClient 渲染 AI 設定 UI（每次連線抓一次）。 |
| 3 | Recorder → wrapper | `POST /SetParameters`（每個 channel 一次） | wrapper 解析解析度、ROI、`ai_settings`；**依 `image_width`/`image_height` 建立／開啟共享記憶體** `ChannelFrame_<port>`，備妥 buffer。回 `200`。 |
| 4 | Recorder → wrapper | 寫入影格（共享記憶體） | Recorder 把 raw I420 寫進 `ChannelFrame_<port>`，設 `image_status = 1`（有新影格）。 |
| 5 | wrapper（內部） | 讀取影格 | wrapper 輪詢到 `image_status == 1`，讀出 I420，設 `image_status = 2`（已取用），送進偵測。 |
| 6 | wrapper → Recorder | `POST {analytics_event_api_url}`（`/PostAnalyticsResult`） | **有偵測到時才送**。body 為結果 JSON（`port_num`、`keyframe`、`timestamp`、`rois_rects`、`items`）。Recorder 的 HTTP 結果 server 收下，依 `port_num` 對應到 channel，推成事件顯示在 Client。 |

指令 1–3 是「連上線」的一次性交握；4–6 是穩態迴圈。

---

## 3. 穩態迴圈（連上線後持續進行）

- **影格**：Recorder 持續寫 MMF（步驟 4）→ wrapper 持續讀並偵測（步驟 5）→ 有結果就 POST（步驟 6）。
- **健康檢查**：Recorder 週期性 `GET /Alive`。連續無回應 → 判定斷線 → 重新 spawn wrapper，回到步驟 1。
- **改設定**：使用者在 ConfigClient 改了 ROI／AI 設定 → Recorder 對該 channel 重送 `POST /SetParameters`（步驟 3）。
- **授權**（選用）：Recorder 可 `GET /GetLicense` 查授權。

---

## 4. 關閉

- Recorder 停用該裝置或程序結束 → 結束 wrapper 程序。
- wrapper 收到關閉 → 停 HTTP listener、釋放共享記憶體對應。

---

## 5. 指令 → 動作 速查

| 指令（方向） | 觸發的動作 |
| --- | --- |
| `GET /Alive`（R→W） | 回健康狀態＋版本；Recorder 用來判斷連線 |
| `GET /GetSettingsSchema`（R→W） | 回設定 schema；Recorder 快取給 UI |
| `POST /SetParameters`（R→W） | 套用設定 + 建立共享記憶體 `ChannelFrame_<port>` |
| 寫 MMF `image_status=1`（R→W） | 通知有新影格 |
| 讀 MMF、設 `image_status=2`（W 內部） | 取走影格、跑偵測 |
| `POST /PostAnalyticsResult`（W→R） | 送出偵測結果；Recorder 依 `port_num` 路由 |
| `GET /GetLicense`（R→W） | 查授權 |

R = Recorder、W = wrapper（`GenericAI.exe`）。

---

## 附：共享記憶體結構

```cpp
constexpr std::int64_t kMmfHeader = 0x1234;
constexpr std::int64_t kMmfFooter = 0x4321;

struct MmfData {
    std::int64_t  header;        // kMmfHeader
    int           image_status;  // 0 = 閒置, 1 = 新影格, 2 = 已取用
    int           image_width;
    int           image_height;
    int           image_size;    // 影格 byte 數
    std::uint64_t timestamp;     // Windows FILETIME（100ns ticks，UTC）
    unsigned char image_data[1920 * 1080 * 3];  // raw I420，上限 1920×1080
    std::int64_t  footer;        // kMmfFooter
};
```

- 區段名稱：`ChannelFrame_<port>`（`<port>` = 該 channel 的控制埠 `exe_server_port + index`）。
- `image_status`：`0` 無資料、`1` 新影格待讀、`2` 已取用。
- SetParameters JSON 與結果 JSON 的完整欄位，見《ARGO 通用 AI 整合指南 v1.3》§3–§4。
