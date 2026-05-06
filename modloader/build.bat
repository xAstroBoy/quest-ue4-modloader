@echo off
setlocal EnableDelayedExpansion
setlocal EnableDelayedExpansion

set "NDK=C:\Android\AndroidNDK\android-ndk-r23c"
if not exist "%NDK%\build\cmake\android.toolchain.cmake" (
    set "NDK=C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r23c"
)
if not exist "%NDK%\build\cmake\android.toolchain.cmake" (
    echo Android NDK r23c not found.
    echo Checked:
    echo   C:\Android\AndroidNDK\android-ndk-r23c
    echo   "C:\Program Files (x86)\Android\AndroidNDK\android-ndk-r23c"
    exit /b 1
)
set ABI=arm64-v8a
set API=24
set "TOOLCHAIN=%NDK%\build\cmake\android.toolchain.cmake"

if not exist build mkdir build
if exist build\CMakeCache.txt del /q build\CMakeCache.txt
if exist build\CMakeFiles rmdir /s /q build\CMakeFiles
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
    echo Unstripped: %CD%\libmodloader.so  (primary artifact + addr2line/crash analysis^)
    echo Unstripped deployment copy: %CD%\deploy\libmodloader.so
    echo.
    echo Use addr2line to symbolicate crash addresses:
    echo   %%NDK%%\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-addr2line -e %CD%\libmodloader.so -f 0xADDRESS
) else (
    echo.
    echo === BUILD FAILED ===
)

cd ..
endlocal
