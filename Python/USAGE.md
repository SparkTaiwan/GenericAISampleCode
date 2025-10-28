# SampleWrapper - Quick Usage Guide

## English Version

### Basic Usage
```bash
# Start with default port 51000
SampleWrapper.exe

# Start with custom port
SampleWrapper.exe port=52000

# Enable debug mode (saves detection images)
SampleWrapper.exe port=51000 debug
```

### Configure Detection
```bash
curl -X POST http://127.0.0.1:51000/SetParameters \
  -H "Content-Type: application/json" \
  -d '{
    "version": "1.2",
    "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
    "image_width": 1280,
    "image_height": 720,
    "jpg_compress": 75,
    "rois": [{
      "sensitivity": 50,
      "threshold": 50, 
      "rects": [
        {"x": 0, "y": 0},
        {"x": 1280, "y": 0},
        {"x": 1280, "y": 720},
        {"x": 0, "y": 720}
      ]
    }]
  }'
```

### Health Check
```bash
curl http://127.0.0.1:51000/Alive
```

---

## 中文版本

### 基本使用
```bash
# 使用預設端口 51000 啟動
SampleWrapper.exe

# 使用自訂端口啟動
SampleWrapper.exe port=52000

# 啟用調試模式（儲存檢測圖片）
SampleWrapper.exe port=51000 debug
```

### 設定檢測參數
```bash
curl -X POST http://127.0.0.1:51000/SetParameters \
  -H "Content-Type: application/json" \
  -d '{
    "version": "1.2",
    "analytics_event_api_url": "http://127.0.0.1:9901/PostAnalyticsResult",
    "image_width": 1280,
    "image_height": 720,
    "jpg_compress": 75,
    "rois": [{
      "sensitivity": 50,
      "threshold": 50, 
      "rects": [
        {"x": 0, "y": 0},
        {"x": 1280, "y": 0},
        {"x": 1280, "y": 720},
        {"x": 0, "y": 720}
      ]
    }]
  }'
```

### 健康檢查
```bash
curl http://127.0.0.1:51000/Alive
```

For detailed documentation, see README.md and README_EN.md
詳細文檔請參考 README.md 和 README_EN.md