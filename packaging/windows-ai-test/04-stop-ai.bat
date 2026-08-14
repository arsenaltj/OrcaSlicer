@echo off
setlocal
cd /d "%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ai\stop_orca_ai_sidecar.ps1" -Endpoint "http://127.0.0.1:18764"
if errorlevel 1 (
    echo Port 18764 is occupied by another process or could not be stopped safely.
    pause
    exit /b 1
)
echo OrcaSlicer AI sidecar stopped.
pause
exit /b 0
