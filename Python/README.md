# SampleWrapper Python 版本

```text
python_version/
├── analytics_engine.py     # 分析引擎 (對應 C++ DLL 功能)
├── data_structures.py      # 數據結構定義 (對應 C# 結構體)
├── detectors.py            # 偵測模組 (YOLO 人體檢測)
├── http_client.py          # HTTP 客戶端 (對應 SimpleHttpClient)
├── http_server.py          # HTTP 服務器 (對應 SimpleHttpServer)
├── image_processor.py      # 圖像處理模組 (YUV420 轉換等)
├── main.py                 # 主程式 (對應 Program.cs)
├── test_yolo_detector.py   # YOLO 檢測器測試
├── build.bat               # Windows 構建腳本
├── SampleWrapper.spec      # PyInstaller 配置文件 (已優化)
├── requirements.txt        # Python 依賴項
├── yolov8n.pt             # YOLO 模型文件
└── README.md              # 說明文件
```

這是原 C#/.NET 和 C++ DLL 專案的 Python 重新實作版本，使用 YOLO 進行人體檢測。

## 專案結構

```text
python_version/
├── analytics_engine.py     # 分析引擎 (對應 C++ DLL 功能)
├── data_structures.py      # 數據結構定義 (對應 C# 結構體)
├── detectors.py            # 偵測模組 (YOLO 人體檢測)
├── http_client.py          # HTTP 客戶端 (對應 SimpleHttpClient)
├── http_server.py          # HTTP 服務器 (對應 SimpleHttpServer)
├── image_processor.py      # 圖像處理模組 (YUV420 轉換等)
├── main.py                 # 主程式 (對應 Program.cs)
├── test_yolo_detector.py   # YOLO 檢測器測試
├── build.bat               # Windows 構建腳本
├── SampleWrapper.spec      # PyInstaller 配置文件 (已優化)
├── requirements.txt        # Python 依賴項
├── yolov8n.pt             # YOLO 模型文件
└── README.md              # 說明文件
```

## 功能對應

### 原 C# 專案功能

- **Program.cs**: 主程式邏輯、DLL 調用、回調處理
- **SimpleHttpClient.cs**: HTTP 客戶端功能
- **SimpleHttpServer.cs**: HTTP 服務器功能
- **C++ DLL**: 共享記憶體處理、圖像分析、回調機制

### Python 重新實作 (輕量級架構)

- **main.py**: 主程式邏輯和異步協調
- **http_client.py**: 輕量級 HTTP 客戶端 (使用 requests)
- **http_server.py**: 輕量級 HTTP 服務器 (使用內建 http.server)
- **analytics_engine.py**: 圖像分析引擎和共享記憶體模擬
- **image_processor.py**: YUV420 到 RGB/JPEG 轉換
- **data_structures.py**: 數據結構定義
- **detectors.py**: YOLO 人體檢測器 (使用 ultralytics)
- **test_yolo_detector.py**: YOLO 檢測器測試工具

## 快速入門範例

### 1. 啟動服務

```bash
# 一般模式
python main.py -port=51000

# Debug 模式（推薦用於測試）
python main.py -port=51000 debug
```

### 2. 設定檢測參數

使用 POST 請求設定檢測參數：

```bash
curl -X POST http://127.0.0.1:51000/SetParameters \
  -H "Content-Type: application/json" \
  -d '{
    "version": "1.2",
    "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
    "image_width": 1280,
    "image_height": 720,
    "jpg_compress": 75,
    "rois": [
      {
        "sensitivity": 70,
        "threshold": 30,
        "rects": [
          {"x": 8, "y": 8},
          {"x": 8, "y": 717},
          {"x": 1277, "y": 8},
          {"x": 1277, "y": 717}
        ]
      }
    ]
  }'
```

### 3. 檢查服務狀態

```bash
# 健康檢查
curl http://127.0.0.1:51000/Alive

# 授權檢查
curl http://127.0.0.1:51000/GetLicense
```

### 4. 觀察檢測結果

- Debug 模式下會顯示詳細檢測日誌
- 檢測到人體時會自動儲存圖片（debug 模式）
- 檢測結果會發送到指定的 analytics_event_api_url

## 安裝和使用

### 方法一：直接運行 Python 程式

#### 安裝依賴

```bash
pip install -r requirements.txt
```

#### 啟動主程式

```bash
# 一般模式
python main.py -port=51000

# Debug 模式 (顯示詳細 log)
python main.py -port=51000 debug
```

#### 運行測試

```bash
# 測試 YOLO 檢測器
python test_yolo_detector.py
```

### 方法二：打包成可執行文件 (推薦)

#### 構建 exe 文件

Windows 用戶可以使用提供的構建腳本：

```batch
# 使用批次文件
build.bat

# 或使用 PowerShell 腳本
.\build.ps1
```

手動構建：

```bash
pip install pyinstaller
pyinstaller SampleWrapper.spec
```

#### 運行 exe 文件

構建完成後，在 `dist` 目錄下會生成：

- `SampleWrapper.exe` - 主可執行文件
- `Usage_Instructions.txt` - 使用說明文件

運行方式：

```bash
# 進入 dist 目錄
cd dist

# 一般模式運行
SampleWrapper.exe -port=51000

# Debug 模式運行 (顯示詳細 log)
SampleWrapper.exe -port=51000 debug
```

### 配置

應用程式使用硬編碼的預設設置：

- **預設端口**: 51000
- **預設 JPG 品質**: 50
- **預設信心閾值**: 25% (會被 SetParameters 覆蓋)
- **主機**: 127.0.0.1

您可以使用命令行參數覆蓋這些設置。

### Debug 模式功能

當啟用 debug 模式時（使用 `debug` 參數），系統會：

- 顯示詳細的檢測日誌
- 當有檢測到人體時，自動儲存帶有檢測框的圖片
- 圖片檔名格式：`debug_detection_YYYYMMDD_HHMMSS_mmm.jpg`
- 顯示 ROI 過濾和閾值轉換的詳細資訊

### ROI 區域過濾

- ROI (Region of Interest) 用來限制檢測範圍
- 4 個座標點定義一個矩形區域的四個角落
- 系統會自動計算矩形的邊界 (min/max x,y)
- 只有在 ROI 區域內的人體檢測才會觸發發報
- 如果沒有設定 ROI，則整個畫面都會進行檢測
- 支援向下相容：也可以用 2 個點定義對角線

## API 端點

### POST /SetParameters

設置分析參數，JSON 格式：

```json
{
  "version": "1.2",
  "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
  "image_width": 1280,
  "image_height": 720,
  "jpg_compress": 75,
  "rois": [
    {
      "sensitivity": 50,
      "threshold": 50,
      "rects": [
        {"x": 8, "y": 8},
        {"x": 8, "y": 717},
        {"x": 1277, "y": 8},
        {"x": 1277, "y": 717}
      ]
    }
  ]
}
```

**參數說明**：

- `version`: API 版本 (固定為 "1.2")
- `analytics_event_api_url`: 檢測結果接收伺服器 URL
- `image_width` / `image_height`: 圖像尺寸
- `jpg_compress`: JPEG 壓縮品質 (1-100)
- `rois`: ROI 區域陣列，每個區域包含：
  - `sensitivity` (0-100): 檢測敏感度，數值越高越容易檢測到人體
  - `threshold` (0-100): 檢測閾值，數值越高檢測越嚴格
  - `rects`: 4 個角點座標，定義矩形檢測區域

**閾值轉換範例**：

- `sensitivity=80, threshold=20` → YOLO confidence ≈ 16% (非常寬鬆)
- `sensitivity=50, threshold=50` → YOLO confidence ≈ 32% (中等)
- `sensitivity=20, threshold=80` → YOLO confidence ≈ 49% (嚴格)

### GET /Alive

健康檢查端點

### GET /GetLicense

授權檢查端點

## 檢測結果回應格式

當檢測到人體時，系統會向設定的 `analytics_event_api_url` 發送檢測結果。

### 回應結構

```json
{
  "version": "1.2",
  "port_num": 1,
  "keyframe": "/9j/4AAQSkZJR...",
  "timestamp": 15003215760000,
  "rois_rects": [
    [
      {"x": 0, "y": 0},
      {"x": 10, "y": 0}, 
      {"x": 10, "y": 10},
      {"x": 0, "y": 10}
    ],
    [
      {"x": 50, "y": 50},
      {"x": 60, "y": 50},
      {"x": 60, "y": 60},
      {"x": 50, "y": 60}
    ]
  ]
}
```

### 參數說明

#### 頂層參數

| 名稱 | 類型 | 範例 | 說明 |
|------|------|------|------|
| version | string | "1.2" | 數據格式版本號，用於相容性追蹤 |
| port_num | int | 1 | 視頻輸入端口或通道識別符 |
| keyframe | string (Base64) | "/9j/4AAQSkZJR..." | 關鍵幀圖像經 Base64 編碼，通常為 JPEG 格式 |
| timestamp | int (epoch-based) | 15003215760000 | 幀的時間戳，單位可為毫秒或微秒，取決於系統 |

#### rois_rects 陣列

`rois_rects` 參數是包含多個矩形 ROI 的陣列。每個 ROI 由四個角點定義。

**ROI 矩形範例**：

```json
[
  {"x": 0, "y": 0},   // 左上角
  {"x": 10, "y": 0},  // 右上角
  {"x": 10, "y": 10}, // 右下角
  {"x": 0, "y": 10}   // 左下角
]
```

**重要說明**：

- `keyframe` 為 JPEG 格式圖像經 Base64 編碼後的字串
- `version` 用於定義傳輸 JSON 的版本號
- 每個檢測到的物體會以 4 個角點表示其邊界框
- 座標系統以圖像左上角為原點 (0,0)

## 主要差異

### 與原 C# 版本的差異

1. **異步處理**: 使用 `asyncio` 替代 C# 的 `async/await`
2. **HTTP 框架**: 使用內建 `http.server` 替代 `HttpListener` (輕量級實作)
3. **HTTP 客戶端**: 使用 `requests` 替代 `HttpClient`
4. **共享記憶體**: 使用檔案映射模擬 Windows 命名共享記憶體
5. **圖像處理**: 使用 `numpy` 和 `PIL` 替代 `System.Drawing`
6. **人體檢測**: 使用 YOLO (ultralytics) 進行人體檢測
7. **參數格式**: 支援更靈活的 JSON 參數格式
8. **ROI 過濾**: 智能區域過濾，只檢測指定區域內的人體
9. **動態閾值**: 可透過 API 動態調整檢測敏感度和閾值

### 與原 C++ DLL 的差異

1. **執行緒管理**: 使用 Python `threading` 替代 C++ `std::thread`
2. **記憶體管理**: Python 自動垃圾回收，無需手動管理
3. **平台相容性**: 可在多平台運行，不限於 Windows
4. **檢測演算法**: 使用現代深度學習模型 (YOLO) 替代傳統電腦視覺方法
5. **即時調整**: 支援執行時動態調整檢測參數，無需重啟
6. **詳細日誌**: 提供豐富的檢測日誌和調試資訊

### 新增功能

1. **智能 ROI 過濾**:
   - 支援多個矩形區域定義
   - 只有在指定區域內的檢測才會觸發告警
   - 大幅減少誤報率

2. **動態閾值轉換**:
   - 將使用者友好的 sensitivity/threshold 參數自動轉換為 YOLO confidence
   - 支援即時調整，無需重新訓練模型

3. **增強的 Debug 模式**:
   - 自動儲存檢測結果圖片
   - 詳細的檢測過程日誌
   - 閾值轉換過程透明化

## 部署和分發

### 部署優勢

1. **單一可執行文件**: 無需安裝 Python 環境
2. **包含所有依賴**: 自包含，減少部署複雜性
3. **跨 Windows 版本**: 相容不同 Windows 版本
4. **命令行配置**: 支援命令行參數配置

### 系統需求

- Windows 7 或更高版本
- 至少 100MB 可用磁碟空間
- 網路連接（如需使用 HTTP 功能）

### 疑難排解

#### 打包問題

1. **缺少模組錯誤**: 檢查 `requirements.txt` 是否包含所有依賴
2. **打包失敗**: 確保有足夠磁碟空間，嘗試更新 PyInstaller
3. **執行時錯誤**: 檢查 `SampleWrapper.spec` 檔案中的 hidden-imports 是否完整
4. **YOLO 模型載入失敗**: 確保 `yolov8n.pt` 檔案存在於專案目錄中

#### 運行問題

1. **端口佔用**: 更改端口號或結束佔用端口的程序
2. **網路錯誤**: 檢查網路連接和防火牆設置
3. **記憶體錯誤**: 確保系統記憶體足夠進行圖像處理和 YOLO 推理
4. **GPU 支援問題**: 如果使用 GPU，確保 CUDA 和 cuDNN 正確安裝

#### 檢測問題

1. **沒有檢測到人體**:
   - 檢查 sensitivity 和 threshold 參數是否太嚴格
   - 使用 debug 模式查看詳細檢測日誌
   - 確認圖像品質和光線條件
   - 檢查 ROI 設定是否正確

2. **檢測結果太多誤報**:
   - 提高 threshold 值（更嚴格）
   - 降低 sensitivity 值（較不敏感）
   - 調整 ROI 區域，排除容易誤報的區域

3. **ROI 過濾不生效**:
   - 確認 JSON 格式正確，rois 陣列包含正確的 ROI 群組結構
   - 每個 ROI 群組的 rects 陣列應包含 4 個角點座標
   - 檢查座標值是否在圖像範圍內
   - 使用 debug 模式確認 ROI 矩形建立成功

4. **參數設定不生效**:
   - 確認 POST /SetParameters 請求成功回應
   - 檢查 JSON 格式是否正確
   - 確認服務器已收到並處理參數

## 依賴項說明

- **requests**: HTTP 客戶端功能
- **Pillow (PIL)**: 圖像處理和格式轉換
- **numpy**: 數值計算和陣列操作
- **torch**: PyTorch 深度學習框架
- **torchvision**: 電腦視覺相關功能
- **ultralytics**: YOLO 檢測模型實現
- **pyinstaller**: 打包成可執行文件

## 模型說明

本專案使用 YOLOv8 Nano 模型 (`yolov8n.pt`) 進行人體檢測：

- **模型大小**: 約 6MB
- **檢測速度**: 快速 (適合即時處理)
- **檢測精度**: 良好的人體檢測準確率
- **硬體需求**: CPU 即可運行，GPU 可加速

如需更高精度，可以替換為：

- `yolov8s.pt` (小型模型)
- `yolov8m.pt` (中型模型)  
- `yolov8l.pt` (大型模型)
- `yolov8x.pt` (超大型模型)
