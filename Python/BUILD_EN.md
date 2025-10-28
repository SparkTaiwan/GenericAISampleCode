# Build Guide

This document provides detailed instructions for packaging the Python Sample Wrapper into a standalone exe executable.

## Prerequisites

1. **Python Environment**: Python 3.8 or higher
2. **Required Tools**: pip (Python package manager)
3. **System**: Windows 7 or higher

## Quick Build

### Automated Build (Recommended)

Use the provided build script:

```batch
# Method 1: Using batch file
build.bat
```

### Manual Build

If automated build fails, you can manually execute the following steps:

1. **Install Dependencies**:
   ```bash
   pip install -r requirements.txt
   pip install pyinstaller
   ```

2. **Clean Previous Builds**:
   ```bash
   rmdir /s /q dist
   rmdir /s /q build
   del *.spec
   ```

3. **Execute Packaging**:
   ```bash
   pyinstaller SampleWrapper.spec
   ```

## Detailed Configuration

### PyInstaller Options Explanation

Important configurations in the `SampleWrapper.spec` file:

- `--onefile`: Package into a single executable file
- `--console`: Show console window (useful for debugging)
- `--add-data`: Include data files (such as templates)
- `--hidden-import`: Include hidden modules needed at runtime
- `--collect-all`: Collect all submodules of specified packages

### Custom Configuration

To modify packaging configuration, edit the `SampleWrapper.spec` file:

```python
# Modify executable file name
name='YourAppName'

# Add icon (if available)
icon='icon.ico'

# Switch to windowed mode (hide console)
console=False

# Add additional data files
datas=[('templates', 'templates')]
```

## Build Output

After successful build, the `dist` directory will contain:

```
dist/
├── SampleWrapper.exe    # Main executable file (approximately 50-80MB)
├── README.md           # Documentation
└── Usage_Instructions.txt # Brief usage instructions
```

## Testing Build Results

1. **Functionality Test**:
   ```bash
   cd dist
   SampleWrapper.exe port=51000
   ```

2. **API Test**:
   ```bash
   # In another terminal window
   curl http://localhost:51000/Alive
   ```

3. **Parameter Test**:
   ```bash
   # Test different ports
   SampleWrapper.exe port=52000
   ```

## Common Issues

### Packaging Errors

**Issue**: `ModuleNotFoundError`
**Solution**: Add missing modules to `hiddenimports` in `SampleWrapper.spec`

**Issue**: File too large
**Solution**: 
- Remove `--collect-all` option
- Use `--exclude-module` to exclude unnecessary modules
- Consider using UPX compression

**Issue**: Slow startup
**Solution**: 
- Reduce the number of packaged modules
- Consider using `--onedir` instead of `--onefile`

### Runtime Errors

**Issue**: Configuration file not found
**Solution**: The application uses hardcoded default configuration, no external config files needed

**Issue**: Port occupied
**Solution**: Use a different port or terminate the occupying process

**Issue**: Firewall blocking
**Solution**: Allow the program through the firewall or run with administrator privileges

## Distribution Recommendations

### Package Distribution

1. **Create Installation Package**:
   ```
   SampleWrapper_v1.0/
   ├── SampleWrapper.exe
   ├── README.md
   ├── Usage_Instructions.txt
   └── install.bat        # Optional installation script
   ```

2. **Compress Files**:
   - Use ZIP or 7z format
   - Include version number and date
   - Example: `SampleWrapper_v1.0_20250910.zip`

   ```bash
   # Using PowerShell compression
   powershell "Compress-Archive -Path * -DestinationPath SampleWrapper-v1.0.0.zip"
   
   # Or using 7-Zip (if installed)
   7z a SampleWrapper-v1.0.0.zip *
   ```

### Version Control

Add version information in `main.py`:

```python
VERSION = "1.0.0"
BUILD_DATE = "2025-09-10"

print(f"Python Sample Wrapper v{VERSION} (Build: {BUILD_DATE})")
```

### Digital Signature (Recommended)

It's recommended to digitally sign the exe file:

```bash
# Using signtool (requires valid code signing certificate)
signtool sign /f certificate.p12 /p password /t http://timestamp.url SampleWrapper.exe
```

## Automated Build

### GitHub Actions Example

Create `.github/workflows/build.yml`:

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

## Advanced Configuration

### Optimizing Build Size

1. **Exclude Unnecessary Modules**:
   ```python
   # In SampleWrapper.spec
   excludes=['tkinter', 'unittest', 'email', 'http', 'urllib', 'xml'],
   ```

2. **Use Minimal PyTorch**:
   ```python
   # Add to hiddenimports for specific modules only
   hiddenimports=[
       'torch._C._nn',
       'torch._C._fft',
       # Add only required torch modules
   ],
   ```

3. **Compress with UPX**:
   ```bash
   # Download UPX and add to PATH
   # Then modify spec file:
   upx=True,
   upx_exclude=[],
   ```

### Debug Build

For debugging purposes, create a debug version:

```python
# Debug version in SampleWrapper.spec
Analysis(
    # ... other options
    debug=True,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
)
```

### Cross-Platform Considerations

While this guide focuses on Windows, for future cross-platform builds:

```python
import sys
import os

# Platform-specific configurations
if sys.platform == 'win32':
    # Windows-specific settings
    console = True
elif sys.platform == 'darwin':
    # macOS-specific settings
    console = False
elif sys.platform.startswith('linux'):
    # Linux-specific settings
    console = True
```

## Performance Optimization

### Build Performance

1. **Parallel Processing**:
   ```bash
   # Use multiple CPU cores for PyInstaller
   pyinstaller --processes 4 SampleWrapper.spec
   ```

2. **Cache Management**:
   ```bash
   # Clean PyInstaller cache if builds are inconsistent
   pyinstaller --clean SampleWrapper.spec
   ```

### Runtime Performance

1. **Startup Optimization**:
   - Minimize imports in main module
   - Use lazy imports where possible
   - Consider splash screen for user feedback

2. **Memory Optimization**:
   - Monitor memory usage with built executable
   - Profile memory leaks in detection loop
   - Consider memory-mapped files for large data

## Troubleshooting Build Issues

### Common PyInstaller Problems

1. **Module Import Errors**:
   ```python
   # Add to spec file hiddenimports
   hiddenimports=[
       'ultralytics.models.yolo',
       'cv2',
       'numpy',
       'PIL',
       # Add any missing modules
   ],
   ```

2. **Data File Issues**:
   ```python
   # Ensure model files are included
   datas=[
       ('yolov8n.pt', '.'),
       ('models/*', 'models'),
   ],
   ```

3. **DLL Dependencies**:
   ```bash
   # Check for missing DLLs
   depends.exe SampleWrapper.exe
   ```

### Testing Checklist

Before distribution, test the executable:

- [ ] Starts without errors
- [ ] HTTP server responds on configured port
- [ ] Parameter setting via API works
- [ ] Detection functionality works
- [ ] Debug mode functions correctly
- [ ] Handles various input scenarios
- [ ] Graceful shutdown on interruption

## Deployment Strategies

### Single File Deployment

Advantages:
- Easy to distribute
- No external dependencies
- Self-contained

Disadvantages:
- Larger file size
- Slower startup
- Temporary file extraction

### Directory Deployment

Advantages:
- Faster startup
- Smaller individual files
- Easier to update components

Disadvantages:
- Multiple files to manage
- Potential for missing files
- More complex distribution

Choose based on your deployment requirements and user preferences.

## Maintenance and Updates

### Version Management

1. **Semantic Versioning**: Use MAJOR.MINOR.PATCH format
2. **Build Numbers**: Include build date and commit hash
3. **Release Notes**: Document changes and improvements

### Update Strategy

1. **Incremental Updates**: For minor changes
2. **Full Replacement**: For major versions
3. **Backward Compatibility**: Maintain config file compatibility

This comprehensive build guide should help you successfully package and distribute the Python Sample Wrapper application.