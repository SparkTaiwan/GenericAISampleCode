@echo off
echo Python Sample Wrapper Build Script
echo ================================

REM Check if Python is installed
python --version >nul 2>&1
if errorlevel 1 (
    echo Error: Python not found, please install Python 3.8 or higher first
    pause
    exit /b 1
)

echo Checking and installing dependencies...
pip install -r requirements.txt
pip install pyinstaller

if errorlevel 1 (
    echo Error: Dependency installation failed
    pause
    exit /b 1
)

REM echo.
REM echo Cleaning previous builds...
REM if exist "dist" rmdir /s /q "dist"
REM if exist "build" rmdir /s /q "build"
REM if exist "*.spec" del /q "*.spec"

echo.
echo Packaging Python Sample Wrapper...
pyinstaller --onefile ^
    --name "SampleWrapper" ^
    --collect-all "numpy" ^
    --collect-all "PIL" ^
    --collect-all "torch" ^
    --collect-all "ultralytics" ^
    --collect-all "cv2" ^
    --hidden-import "torch" ^
    --hidden-import "torchvision" ^
    --hidden-import "ultralytics" ^
    --hidden-import "cv2" ^
    --hidden-import "multiprocessing" ^
    --hidden-import "ctypes" ^
    --hidden-import "threading" ^
    --hidden-import "mmap" ^
    --hidden-import "struct" ^
    --hidden-import "base64" ^
    --hidden-import "io" ^
    --hidden-import "asyncio" ^
    --hidden-import "copy" ^
    --console ^
    main.py

if errorlevel 1 (
    echo Error: Packaging failed
    pause
    exit /b 1
)

echo.
echo Packaging successful!
echo Executable location: dist\SampleWrapper.exe

REM Copy documentation files to dist directory
copy "README.md" "dist\" >nul

echo Documentation files copied to dist directory

REM Create usage instructions
echo # SampleWrapper Usage Instructions > "dist\Usage_Instructions.txt"
echo. >> "dist\Usage_Instructions.txt"
echo ## How to Run >> "dist\Usage_Instructions.txt"
echo SampleWrapper.exe port=51000 >> "dist\Usage_Instructions.txt"
echo SampleWrapper.exe port=51000 debug  (Enable debug mode) >> "dist\Usage_Instructions.txt"
echo. >> "dist\Usage_Instructions.txt"
echo ## Configuration >> "dist\Usage_Instructions.txt"
echo - Default Port: 51000 >> "dist\Usage_Instructions.txt"
echo - Default JPG Quality: 50 >> "dist\Usage_Instructions.txt"
echo - Use command line arguments to override defaults >> "dist\Usage_Instructions.txt"
echo. >> "dist\Usage_Instructions.txt"
echo ## API Endpoints >> "dist\Usage_Instructions.txt"
echo - POST /SetParameters: Set analysis parameters >> "dist\Usage_Instructions.txt"
echo - GET /Alive: Health check >> "dist\Usage_Instructions.txt"
echo - GET /GetLicense: License check >> "dist\Usage_Instructions.txt"
echo. >> "dist\Usage_Instructions.txt"
echo ## Debug Mode >> "dist\Usage_Instructions.txt"
echo - Add debug parameter to show detailed logs >> "dist\Usage_Instructions.txt"
echo - Triggers detection every 60 cycles (simulates C++ DLL logic) >> "dist\Usage_Instructions.txt"
echo - Shows shared memory data reading details >> "dist\Usage_Instructions.txt"

echo Usage instructions file created
echo.
echo Build completed! Please check the dist directory
pause
