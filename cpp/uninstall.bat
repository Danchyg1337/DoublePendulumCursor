@echo off
REM Double-click to remove autostart and stop the app.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1"
echo.
pause
