@echo off
setlocal
cd /d "%~dp0"
set "ORCASLICER_AI_SIDECAR_PORT=18764"
set "ORCASLICER_AI_SIDECAR_URL=http://127.0.0.1:%ORCASLICER_AI_SIDECAR_PORT%"
start "OrcaSlicer AI sidecar" "%~dp0tools\ai\start_orca_ai_sidecar.bat"
start "OrcaSlicer" "%~dp0build\OrcaSlicer\orca-slicer.exe"
