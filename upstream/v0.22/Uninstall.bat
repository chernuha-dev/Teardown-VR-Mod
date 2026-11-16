@echo off
rem Teardown VR Mod - double-click to uninstall (runs uninstall_release.ps1 next to this file).
if not exist "%~dp0uninstall_release.ps1" (echo Extract the whole zip first, then run Uninstall.bat from the extracted folder.& echo.& pause& exit /b 1)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall_release.ps1" %*
echo.
pause
