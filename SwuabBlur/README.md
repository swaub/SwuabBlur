# SwuabBlur - Compilation Fixes

This document addresses the compilation errors you encountered and provides the fixes.

## Issues Fixed

### 1. Struct Option Redefinition (C2011)
**Problem**: `struct option` was defined in both `getopt.h` and `config.c`, causing a redefinition error.

**Fix**: Removed the duplicate `struct option` definition from `config.c`. The definition in `getopt.h` is sufficient.

### 2. Deprecated strdup Function (C4996)
**Problem**: Windows deprecated `strdup` in favor of `_strdup`.

**Fix**: Added `#define strdup _strdup` for Windows builds in the fixed `config.c`.

### 3. Missing FFmpeg Headers (C1083)
**Problem**: FFmpeg development libraries not installed or not in the correct location.

**Solution**: 
1. Download FFmpeg development libraries from [BtbN FFmpeg Builds](https://github.com/BtbN/FFmpeg-Builds/releases)
2. Extract to `C:\ffmpeg`
3. Ensure the directory structure is:
   ```
   C:\ffmpeg\
   ├── include\
   │   ├── libavcodec\
   │   ├── libavformat\
   │   └── ...
   └── lib\
   ```

## Files Modified

- **config.c**: Fixed struct redefinition and strdup issues
- **setup_environment.bat**: Enhanced dependency checking
- Added comprehensive setup guide

## Quick Fix Steps

1. **Replace config.c** with the fixed version provided
2. **Download FFmpeg**: Get the latest Windows build and extract to `C:\ffmpeg`
3. **Run setup_environment.bat** to verify all dependencies
4. **Build the project** in Release x64 configuration

## Dependencies Required

| Component | Location | Required |
|-----------|----------|----------|
| FFmpeg Dev Libraries | `C:\ffmpeg` | ✅ Yes |
| VapourSynth SDK | `C:\Program Files\VapourSynth` | ❌ Optional |
| pthreads-win32 | `third_party\pthreads` | ✅ Yes |

## Build Configuration

- **Platform**: x64 (required)
- **Configuration**: Release (recommended)
- **Toolset**: v143 (Visual Studio 2022)

## Testing

After fixing and building:

```cmd
SwuabBlur.exe --help
SwuabBlur.exe -o test.mp4 --blur-amount 1.0 input.mp4
```

## Additional Notes

- Ensure all FFmpeg DLLs are available at runtime
- Add `C:\ffmpeg\bin` to your system PATH
- Use the enhanced setup script for automated dependency checking
- See the full Setup Guide for detailed instructions

The fixed code maintains all original functionality while resolving Windows compilation issues.