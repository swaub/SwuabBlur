@echo off
echo =====================================================
echo SwuabBlur Development Environment Setup
echo =====================================================
echo.

set SCRIPT_DIR=%~dp0
set FFMPEG_DIR=C:\ffmpeg
set VAPOURSYNTH_DIR=C:\Program Files\VapourSynth
set THIRD_PARTY_DIR=%SCRIPT_DIR%third_party

echo Creating directory structure...
if not exist "%THIRD_PARTY_DIR%" mkdir "%THIRD_PARTY_DIR%"
if not exist "%THIRD_PARTY_DIR%\getopt" mkdir "%THIRD_PARTY_DIR%\getopt"
if not exist "%THIRD_PARTY_DIR%\pthreads" mkdir "%THIRD_PARTY_DIR%\pthreads"
if not exist "%THIRD_PARTY_DIR%\pthreads\include" mkdir "%THIRD_PARTY_DIR%\pthreads\include"
if not exist "%THIRD_PARTY_DIR%\pthreads\lib" mkdir "%THIRD_PARTY_DIR%\pthreads\lib"
echo Done.
echo.

echo Checking dependencies...
echo.

echo [1/3] Checking FFmpeg...
if exist "%FFMPEG_DIR%\include\libavcodec\avcodec.h" (
    echo     [OK] FFmpeg development libraries found
    if exist "%FFMPEG_DIR%\bin\ffmpeg.exe" (
        echo     [OK] FFmpeg binaries found
    ) else (
        echo     [WARNING] FFmpeg binaries not found in bin directory
    )
) else (
    echo     [ERROR] FFmpeg not found at %FFMPEG_DIR%
    echo     Please download from: https://github.com/BtbN/FFmpeg-Builds/releases
    echo     Extract ffmpeg-master-latest-win64-gpl-shared.zip to C:\ffmpeg
    set MISSING_DEPS=1
)
echo.

echo [2/3] Checking VapourSynth (Optional)...
if exist "%VAPOURSYNTH_DIR%\sdk\include\VapourSynth.h" (
    echo     [OK] VapourSynth SDK found
) else (
    echo     [INFO] VapourSynth not found - this is optional
    echo     Download from: https://github.com/vapoursynth/vapoursynth/releases
)
echo.

echo [3/3] Checking pthreads-win32...
if exist "%THIRD_PARTY_DIR%\pthreads\include\pthread.h" (
    echo     [OK] pthreads headers found
) else (
    echo     [ERROR] pthreads headers not found
    echo     Please download pthreads-win32 from SourceForge
    echo     Extract to: %THIRD_PARTY_DIR%\pthreads\
    set MISSING_DEPS=1
)

if exist "%THIRD_PARTY_DIR%\pthreads\lib\pthreadVC2.lib" (
    echo     [OK] pthreads library found
) else (
    echo     [ERROR] pthreads library not found
    echo     Ensure pthreadVC2.lib is in: %THIRD_PARTY_DIR%\pthreads\lib\
    set MISSING_DEPS=1
)
echo.

echo Checking getopt.h...
if exist "%THIRD_PARTY_DIR%\getopt\getopt.h" (
    echo     [OK] getopt.h found
) else (
    echo     [INFO] Copying getopt.h to third_party directory...
    if exist "%SCRIPT_DIR%SwuabBlur\getopt.h" (
        copy "%SCRIPT_DIR%SwuabBlur\getopt.h" "%THIRD_PARTY_DIR%\getopt\" >nul
        echo     [OK] getopt.h copied successfully
    ) else (
        echo     [ERROR] getopt.h not found in project directory
        set MISSING_DEPS=1
    )
)
echo.

echo Checking PATH environment...
echo %PATH% | findstr /i "ffmpeg" >nul
if %ERRORLEVEL% == 0 (
    echo     [OK] FFmpeg found in PATH
) else (
    echo     [WARNING] FFmpeg not in PATH
    echo     Add C:\ffmpeg\bin to your system PATH for runtime
)
echo.

echo =====================================================
echo Setup Summary
echo =====================================================

if defined MISSING_DEPS (
    echo Status: INCOMPLETE - Missing required dependencies
    echo.
    echo Required Actions:
    echo 1. Download and install missing dependencies listed above
    echo 2. Run this script again to verify setup
    echo 3. Open SwuabBlur.sln in Visual Studio
    echo.
    echo Download Links:
    echo - FFmpeg: https://github.com/BtbN/FFmpeg-Builds/releases
    echo - pthreads: https://sourceforge.net/projects/pthreads-win32/
) else (
    echo Status: READY
    echo.
    echo Next Steps:
    echo 1. Open SwuabBlur.sln in Visual Studio
    echo 2. Ensure build configuration is set to Release x64
    echo 3. Build the solution (Ctrl+Shift+B)
    echo 4. Copy FFmpeg DLLs from C:\ffmpeg\bin to output directory
    echo.
    echo Quick Test:
    echo SwuabBlur.exe -o test.mp4 --blur-amount 1.0 input.mp4
)

echo.
echo For detailed setup instructions, see the Setup Guide.
echo.
pause