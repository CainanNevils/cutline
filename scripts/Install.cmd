@echo off
rem Double-click to install Cutline for this user (no admin needed) and start it.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Launch
if errorlevel 1 pause
