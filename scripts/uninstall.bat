@echo off
rem Removes the PAA WIC codec. Run as administrator.
setlocal

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\PaaViewer"

if exist "%APPDIR%\PaaWic.dll" (
  regsvr32 /s /u "%APPDIR%\PaaWic.dll"
  rem Explorer may still hold the DLL; it can then be deleted after a restart.
  del /f "%APPDIR%\PaaWic.dll" 2>nul
  if exist "%APPDIR%\PaaWic.dll" echo PaaWic.dll is in use, delete "%APPDIR%" after a restart.
  rmdir "%APPDIR%" 2>nul
  echo Windows codec removed.
)
