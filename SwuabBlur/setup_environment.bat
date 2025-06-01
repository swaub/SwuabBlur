@echo off
echo Setting up SwuabBlur development environment...

REM Create third-party directory structure
if not exist "third_party" mkdir third_party
if not exist "third_party\getopt" mkdir third_party\getopt
if not exist "third_party\pthreads" mkdir third_party\pthreads

echo.
echo Please ensure you have downloaded and installed:
echo 1. FFmpeg development libraries to C:\ffmpeg
echo 2. VapourSynth to C:\Program Files\VapourSynth
echo 3. pthreads-win32 to third_party\pthreads
echo.

echo Checking for required directories...

if exist "C:\ffmpeg\include" (
    echo [OK] FFmpeg found at C:\ffmpeg
) else (
    echo [ERROR] FFmpeg not found at C:\ffmpeg
    echo Download from: https://github.com/BtbN/FFmpeg-Builds/releases
)

if exist "C:\Program Files\VapourSynth\sdk\include" (
    echo [OK] VapourSynth found
) else (
    echo [ERROR] VapourSynth not found
    echo Download from: https://github.com/vapoursynth/vapoursynth/releases
)

echo.
echo Next steps:
echo 1. Copy getopt.h to third_party\getopt\
echo 2. Download pthreads-win32 to third_party\pthreads\
echo 3. Update Visual Studio project properties as shown in the setup guide
echo.
pause