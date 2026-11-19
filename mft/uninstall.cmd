@echo off
rem uninstall.cmd - remove the DroidVM virtio-media H.264 MFT(s).
rem Run from an elevated command prompt.  Each /u removes the COM keys, the
rem MediaFoundation\Transforms entry and the Categories link from the native
rem and WOW6432Node views (the x86 DLL's /u is a harmless second pass).
setlocal
set "D=%~dp0"
set "REG64=%SystemRoot%\System32\regsvr32.exe"
set "REG32=%SystemRoot%\SysWOW64\regsvr32.exe"

call :unregister "%D%mft_h264_decoder_x86.dll"   "%REG32%"
call :unregister "%D%mft_h264_encoder_x86.dll"   "%REG32%"
call :unregister "%D%mft_h264_decoder_arm64.dll" "%REG64%"
call :unregister "%D%mft_h264_encoder_arm64.dll" "%REG64%"
echo [mft] unregistered (Microsoft's inbox decoders remain available)
exit /b %ERRORLEVEL%

:unregister
if not exist "%~1" exit /b 0
if not exist "%~2" exit /b 0
echo [mft] unregister %~nx1
"%~2" /u /s "%~1"
if errorlevel 1 (
  echo [mft] regsvr32 /u failed for %~nx1, errorlevel=%errorlevel%
  exit /b 1
)
exit /b 0
