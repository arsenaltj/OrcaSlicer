@echo off
setlocal
cd /d "%~dp0"

set "ORCASLICER_AI_SIDECAR_PORT=18764"
set "ORCASLICER_AI_SIDECAR_URL=http://127.0.0.1:%ORCASLICER_AI_SIDECAR_PORT%"

if not exist "%~dp0build\OrcaSlicer\orca-slicer.exe" (
    echo OrcaSlicer was not found at build\OrcaSlicer\orca-slicer.exe.
    echo Build and install the Release configuration first.
    exit /b 1
)

start "OrcaSlicer AI sidecar" "%~dp0tools\ai\start_orca_ai_sidecar.bat"

set /a AI_SIDECAR_ATTEMPT=0
:wait_for_ai_sidecar
powershell.exe -NoProfile -Command "$health = Invoke-RestMethod -Uri '%ORCASLICER_AI_SIDECAR_URL%/health' -TimeoutSec 1 -ErrorAction SilentlyContinue; if ($health.ok -eq $true) { exit 0 }; exit 1" >nul 2>nul
if not errorlevel 1 goto ai_sidecar_ready
set /a AI_SIDECAR_ATTEMPT+=1
if %AI_SIDECAR_ATTEMPT% GEQ 15 goto ai_sidecar_timeout
timeout /t 1 /nobreak >nul
goto wait_for_ai_sidecar

:ai_sidecar_ready
echo AI sidecar is ready.
goto launch_orcaslicer

:ai_sidecar_timeout
echo AI sidecar did not become ready within 15 seconds. OrcaSlicer will continue and retry discovery.

:launch_orcaslicer
start "OrcaSlicer" "%~dp0build\OrcaSlicer\orca-slicer.exe"
