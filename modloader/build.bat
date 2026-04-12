@echo off
setlocal

set "NDK=C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r23c"
set ABI=arm64-v8a
set API=24
set "TOOLCHAIN=%NDK%\build\cmake\android.toolchain.cmake"

if not exist build mkdir build
cd build

cmake -G Ninja ^
    "-DCMAKE_TOOLCHAIN_FILE=%TOOLCHAIN%" ^
    -DANDROID_ABI=%ABI% ^
    -DANDROID_PLATFORM=android-%API% ^
    -DANDROID_STL=c++_static ^
    -DCMAKE_BUILD_TYPE=Release ^
    ..

ninja -j%NUMBER_OF_PROCESSORS%

if %ERRORLEVEL% EQU 0 (
    echo.
    echo === BUILD SUCCEEDED ===
    echo Output: %CD%\libmodloader.so
) else (
    echo.
    echo === BUILD FAILED ===
)

cd ..
endlocal
