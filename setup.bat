@echo off
rem Double-click me. See docs\INSTALL.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup.ps1" %*
echo.
pause
