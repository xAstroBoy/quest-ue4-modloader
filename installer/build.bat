@echo off
echo ================================
echo  UE Modloader Installer - Build
echo ================================
echo.

cd /d "%~dp0"
cargo build --release
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    pause
    exit /b 1
)

echo.
echo ====================================
echo  Built: target\release\ue-modloader-installer.exe
echo ====================================
echo.
echo Usage:
echo   GUI mode:  ue-modloader-installer.exe
echo   CLI mode:  ue-modloader-installer.exe --cli
echo.
pause
