@echo off
rem Installs the PAA WIC codec (Windows Explorer thumbnails for .paa files).
rem Run as administrator from the release folder (PaaWic.dll next to this file).
setlocal
cd /d "%~dp0"

net session >nul 2>&1 || (
  echo Please run this file as administrator.
  exit /b 1
)
if not exist PaaWic.dll (
  echo PaaWic.dll not found next to install.bat. Build the project first, then run dist\install.bat.
  exit /b 1
)

set "APPDIR=%ProgramFiles%\PaaViewer"

if not exist "%APPDIR%" mkdir "%APPDIR%"
if exist "%APPDIR%\PaaWic.dll" regsvr32 /s /u "%APPDIR%\PaaWic.dll"
copy /y PaaWic.dll "%APPDIR%\PaaWic.dll" >nul || (
  echo Could not replace "%APPDIR%\PaaWic.dll": it is probably in use by Explorer.
  echo Run uninstall.bat, sign out or restart, then run install.bat again.
  exit /b 1
)
regsvr32 /s "%APPDIR%\PaaWic.dll" || (
  echo Codec registration failed. If Microsoft Defender attack surface reduction rules are
  echo enforced on this PC, allow the DLL first ^(admin PowerShell^):
  echo   Add-MpPreference -AttackSurfaceReductionOnlyExclusions "%APPDIR%\PaaWic.dll"
  exit /b 1
)
echo Windows codec installed: %APPDIR%\PaaWic.dll
echo.
echo Done. If .paa files still show blank icons, run clear-thumbnails.bat.
