@echo off
setlocal EnableDelayedExpansion

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
set BUILD_RESULT=!ERRORLEVEL!

if !BUILD_RESULT! EQU 0 (
    echo.
    echo === BUILD SUCCEEDED ===
    echo Unstripped: %CD%\libmodloader.so  (for addr2line/crash analysis^)
    echo Stripped:   %CD%\deploy\libmodloader.so  (for device deployment^)
    echo.
    echo Use addr2line to symbolicate crash addresses:
    echo   %%NDK%%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-addr2line -e %CD%\libmodloader.so -f 0xADDRESS
) else (
    echo.
    echo === BUILD FAILED ===
)

cd ..
endlocal
