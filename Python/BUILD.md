# 構建指南

本文檔詳細說明如何將 Python Sample Wrapper 打包成獨立的 exe 可執行文件。

## 前置條件

1. **Python 環境**: Python 3.8 或更高版本
2. **必要工具**: pip（Python 套件管理器）
3. **系統**: Windows 7 或更高版本

## 快速構建

### 自動構建（推薦）

使用提供的構建腳本：

```batch
# 方法一：使用批次文件
build.bat

```

### 手動構建

如果自動構建失敗，可以手動執行以下步驟：

1. **安裝依賴**：
   ```bash
   pip install -r requirements.txt
   pip install pyinstaller
   ```

2. **清理舊構建**：
   ```bash
   rmdir /s /q dist
   rmdir /s /q build
   del *.spec
   ```

3. **執行打包**：
   ```bash
   pyinstaller SampleWrapper.spec
   ```

## 詳細配置

### PyInstaller 選項說明

在 `SampleWrapper.spec` 文件中的重要配置：

- `--onefile`: 打包成單一可執行文件
- `--console`: 顯示控制台視窗（便於調試）
- `--add-data`: 包含數據文件（如模板等）
- `--hidden-import`: 包含運行時需要的隱藏模組
- `--collect-all`: 收集指定套件的所有子模組

### 自定義配置

如需修改打包配置，編輯 `SampleWrapper.spec` 文件：

```python
# 修改可執行文件名稱
name='YourAppName'

# 添加圖標（如果有）
icon='icon.ico'

# 修改為視窗模式（隱藏控制台）
console=False

# 添加額外的數據文件
datas=[('templates', 'templates')]
```

## 構建輸出

成功構建後，`dist` 目錄將包含：

```
dist/
├── SampleWrapper.exe    # 主可執行文件（約 50-80MB）
├── README.md           # 說明文件
└── 使用說明.txt         # 簡要使用說明
```

## 測試構建結果

1. **功能測試**：
   ```bash
   cd dist
   SampleWrapper.exe port=51000
   ```

2. **API 測試**：
   ```bash
   # 在另一個終端視窗
   curl http://localhost:51000/Alive
   ```

3. **參數測試**：
   ```bash
   # 測試不同端口
   SampleWrapper.exe port=52000
   ```

## 常見問題

### 打包錯誤

**問題**: `ModuleNotFoundError`
**解決**: 在 `SampleWrapper.spec` 的 `hiddenimports` 中添加缺少的模組

**問題**: 文件過大
**解決**: 
- 移除 `--collect-all` 選項
- 使用 `--exclude-module` 排除不需要的模組
- 考慮使用 UPX 壓縮

**問題**: 啟動慢
**解決**: 
- 減少打包的模組數量
- 考慮使用 `--onedir` 而非 `--onefile`

### 運行錯誤

**問題**: 配置文件未找到
**解決**: 應用程式使用硬編碼的預設配置，無需外部配置文件

**問題**: 端口被佔用
**解決**: 使用不同端口或結束佔用進程

**問題**: 防火牆阻攔
**解決**: 允許程式通過防火牆或使用管理員權限運行

## 分發建議

### 打包分發

1. **創建安裝包**：
   ```
   SampleWrapper_v1.0/
   ├── SampleWrapper.exe
   ├── README.md
   ├── 使用說明.txt
   └── install.bat        # 可選的安裝腳本
   ```

2. **壓縮檔案**：
   - 使用 ZIP 或 7z 格式
   - 包含版本號和日期
   - 例：`SampleWrapper_v1.0_20250910.zip`

   ```bash
   # 使用 PowerShell 壓縮
   powershell "Compress-Archive -Path * -DestinationPath SampleWrapper-v1.0.0.zip"
   
   # 或使用 7-Zip（如果已安裝）
   7z a SampleWrapper-v1.0.0.zip *
   ```

### 版本控制

在 `main.py` 中添加版本信息：

```python
VERSION = "1.0.0"
BUILD_DATE = "2025-09-10"

print(f"Python Sample Wrapper v{VERSION} (Build: {BUILD_DATE})")
```

### 數位簽章（建議使用)

建議對 exe 文件進行數位簽章：

```bash
# 使用 signtool（需要有效的程式碼簽章憑證）
signtool sign /f certificate.p12 /p password /t http://timestamp.url SampleWrapper.exe
```

## 自動化構建

### GitHub Actions 範例

創建 `.github/workflows/build.yml`：

```yaml
name: Build Executable

on:
  push:
    tags:
      - 'v*'

jobs:
  build:
    runs-on: windows-latest
    
    steps:
    - uses: actions/checkout@v2
    
    - name: Set up Python
      uses: actions/setup-python@v2
      with:
        python-version: '3.9'
    
    - name: Install dependencies
      run: |
        pip install -r requirements.txt
        pip install pyinstaller
    
    - name: Build executable
      run: pyinstaller SampleWrapper.spec
    
    - name: Upload artifact
      uses: actions/upload-artifact@v2
      with:
        name: SampleWrapper-Windows
        path: dist/
```
