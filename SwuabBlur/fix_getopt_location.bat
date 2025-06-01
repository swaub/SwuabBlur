@echo off
echo Fixing getopt.h location for SwuabBlur...
echo.

set TARGET_DIR=C:\Users\Brendan\source\repos\SwuabBlur\SwuabBlur\third_party\getopt
set SOURCE_FILE=C:\Users\Brendan\source\repos\SwuabBlur\SwuabBlur\getopt.h

echo Creating target directory: %TARGET_DIR%
if not exist "%TARGET_DIR%" mkdir "%TARGET_DIR%"

echo Copying getopt.h to correct location...
if exist "%SOURCE_FILE%" (
    copy "%SOURCE_FILE%" "%TARGET_DIR%\" >nul
    if exist "%TARGET_DIR%\getopt.h" (
        echo [OK] getopt.h successfully copied to:
        echo     %TARGET_DIR%\getopt.h
    ) else (
        echo [ERROR] Failed to copy getopt.h
    )
) else (
    echo [ERROR] Source file not found: %SOURCE_FILE%
    echo.
    echo Please check if getopt.h exists in your SwuabBlur directory
)

echo.
echo Verifying final location...
if exist "%TARGET_DIR%\getopt.h" (
    echo [SUCCESS] getopt.h is now in the correct location!
    echo.
    echo You can now:
    echo 1. Run setup_environment.bat to verify
    echo 2. Open SwuabBlur.sln in Visual Studio
    echo 3. Build the project
) else (
    echo [FAILED] getopt.h is still not in the correct location
)

echo.
pause