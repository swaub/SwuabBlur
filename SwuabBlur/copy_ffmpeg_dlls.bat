@echo off
echo Copying FFmpeg DLLs to SwuabBlur output directories...

set FFMPEG_BIN=C:\ffmpeg\bin
set PROJECT_ROOT=C:\Users\Brendan\source\repos\SwuabBlur

echo.
echo Copying to Debug x64...
if not exist "%PROJECT_ROOT%\x64\Debug" mkdir "%PROJECT_ROOT%\x64\Debug"
copy "%FFMPEG_BIN%\avcodec-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1
copy "%FFMPEG_BIN%\avformat-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1
copy "%FFMPEG_BIN%\avutil-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1
copy "%FFMPEG_BIN%\avfilter-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1
copy "%FFMPEG_BIN%\swscale-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1
copy "%FFMPEG_BIN%\swresample-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1
copy "%FFMPEG_BIN%\postproc-*.dll" "%PROJECT_ROOT%\x64\Debug\" >nul 2>&1

echo Copying to Release x64...
if not exist "%PROJECT_ROOT%\x64\Release" mkdir "%PROJECT_ROOT%\x64\Release"
copy "%FFMPEG_BIN%\avcodec-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1
copy "%FFMPEG_BIN%\avformat-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1
copy "%FFMPEG_BIN%\avutil-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1
copy "%FFMPEG_BIN%\avfilter-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1
copy "%FFMPEG_BIN%\swscale-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1
copy "%FFMPEG_BIN%\swresample-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1
copy "%FFMPEG_BIN%\postproc-*.dll" "%PROJECT_ROOT%\x64\Release\" >nul 2>&1

echo.
echo Verifying DLLs copied...
if exist "%PROJECT_ROOT%\x64\Debug\avcodec-*.dll" (
    echo [OK] Debug DLLs copied successfully
) else (
    echo [ERROR] Debug DLLs not found
)

if exist "%PROJECT_ROOT%\x64\Release\avcodec-*.dll" (
    echo [OK] Release DLLs copied successfully  
) else (
    echo [ERROR] Release DLLs not found
)

echo.
echo DLL copy complete! You can now run SwuabBlur.exe
echo.
pause