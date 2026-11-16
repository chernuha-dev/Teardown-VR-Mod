@echo off
rem Teardown VR Mod - double-click to install (runs install_release.ps1 next to this file).
if not exist "%~dp0install_release.ps1" (echo Extract the whole zip first, then run Install.bat from the extracted folder.& echo.& pause& exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install_release.ps1" %*
echo.
pause
