@echo off
echo =====================================================
echo SwuabBlur Development Environment Setup - FIXED
echo =====================================================
echo.

set PROJECT_ROOT=C:\Users\Brendan\source\repos\SwuabBlur
set SWUAB_DIR=%PROJECT_ROOT%\SwuabBlur
set FFMPEG_DIR=C:\ffmpeg
set VAPOURSYNTH_DIR=C:\Program Files\VapourSynth
set THIRD_PARTY_DIR=%SWUAB_DIR%\third_party

echo Creating directory structure...
if not exist "%THIRD_PARTY_DIR%" mkdir "%THIRD_PARTY_DIR%"
if not exist "%THIRD_PARTY_DIR%\getopt" mkdir "%THIRD_PARTY_DIR%\getopt"
echo Done.
echo.

echo Fixing getopt.h location...
if exist "%SWUAB_DIR%\getopt.h" (
    copy "%SWUAB_DIR%\getopt.h" "%THIRD_PARTY_DIR%\getopt\" >nul
    echo     [OK] getopt.h copied to correct location
) else (
    echo     [INFO] getopt.h not found in SwuabBlur directory
    if exist "%THIRD_PARTY_DIR%\getopt\getopt.h" (
        echo     [OK] getopt.h already exists in target location
    ) else (
        echo     [ERROR] getopt.h not found anywhere
    )
)
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

echo [2/3] Checking VapourSynth...
if exist "%VAPOURSYNTH_DIR%\sdk\include\vapoursynth\VapourSynth.h" (
    echo     [OK] VapourSynth SDK headers found
    if exist "%VAPOURSYNTH_DIR%\sdk\lib64" (
        echo     [OK] VapourSynth SDK libraries found
    ) else (
        echo     [WARNING] VapourSynth lib64 directory not found
    )
    if exist "%VAPOURSYNTH_DIR%\sdk\examples" (
        echo     [OK] VapourSynth examples found
    )
) else (
    echo     [INFO] VapourSynth not found - this is optional
    echo     Checked: %VAPOURSYNTH_DIR%\sdk\include\vapoursynth\VapourSynth.h
    echo     Download from: https://github.com/vapoursynth/vapoursynth/releases
)
echo.

echo [3/3] Checking getopt.h final location...
if exist "%THIRD_PARTY_DIR%\getopt\getopt.h" (
    echo     [OK] getopt.h found at: %THIRD_PARTY_DIR%\getopt\getopt.h
) else (
    echo     [ERROR] getopt.h not found at expected location
    echo     Expected: %THIRD_PARTY_DIR%\getopt\getopt.h
    set MISSING_DEPS=1
)
echo.

echo Note: pthreads-win32 is not required - using Windows threading
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
echo VapourSynth Installation Details:
if exist "%VAPOURSYNTH_DIR%\sdk" (
    echo SDK Path: %VAPOURSYNTH_DIR%\sdk
    echo Include Path: %VAPOURSYNTH_DIR%\sdk\include\vapoursynth
    echo Library Path: %VAPOURSYNTH_DIR%\sdk\lib64
) else (
    echo VapourSynth SDK not detected
)
echo.
echo For detailed setup instructions, see the Setup Guide.
echo.
pause