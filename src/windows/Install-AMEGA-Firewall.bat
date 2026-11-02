@echo off
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Allow-AMEGA-Firewall.ps1"
if errorlevel 1 (
  echo.
  echo Firewall setup reported an error. Read the message above.
  pause
)
