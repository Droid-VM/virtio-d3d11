@echo off
rem install.cmd - register the DroidVM virtio-media H.264 MFT(s).
rem
rem Run from an elevated command prompt.  Each DLL registers:
rem   - HKLM\SOFTWARE\Classes\CLSID\{...}\InprocServer32
rem   - HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\{...}
rem       with MFTFlags=4, the AFMI attributes blob and MFT_ENUM_HARDWARE_*
rem   - HKLM\SOFTWARE\Classes\MediaFoundation\Transforms\Categories\{...}\{...}
rem The ARM64 DLL also mirrors its registration into WOW6432Node when
rem mft_h264_*_x86.dll sits next to it; the x86 regsvr32 below is the
rem authoritative path for the 32-bit view and is harmless if already done.
rem
rem Decoder and encoder are independent components (different CLSIDs and
rem categories); install either or both.
setlocal
set "D=%~dp0"
set "REG64=%SystemRoot%\System32\regsvr32.exe"
set "REG32=%SystemRoot%\SysWOW64\regsvr32.exe"

call :register "%D%mft_h264_decoder_arm64.dll" "%REG64%"
call :register "%D%mft_h264_decoder_x86.dll"   "%REG32%"
call :register "%D%mft_h264_encoder_arm64.dll" "%REG64%"
call :register "%D%mft_h264_encoder_x86.dll"   "%REG32%"

echo.
echo [mft] verify with:
echo       powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-mft-reg.ps1" -Mode decoder
echo       powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-mft-reg.ps1" -Mode encoder
echo       mft_enum_probe_arm64.exe           (decoder, expects HARDWARE_FOUND=1)
echo       mft_enum_probe_encoder_arm64.exe   (encoder skeleton, expects HARDWARE_FOUND=1)
exit /b %ERRORLEVEL%

:register
if not exist "%~1" (
  echo [mft] skip missing %~nx1
  exit /b 0
)
if not exist "%~2" (
  echo [mft] skip %~nx1: no %~2
  exit /b 0
)
echo [mft] register %~nx1 with %~2
"%~2" /s "%~1"
if errorlevel 1 (
  echo [mft] regsvr32 failed for %~nx1, errorlevel=%errorlevel% ^(elevated?^)
  exit /b 1
)
exit /b 0
