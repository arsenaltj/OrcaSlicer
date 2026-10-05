@echo off
setlocal
powershell.exe -STA -NoProfile -ExecutionPolicy Bypass -File "%~dp0evaluator_trial_launcher.ps1"
if errorlevel 1 pause
