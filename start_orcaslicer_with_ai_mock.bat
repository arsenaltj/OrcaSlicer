@echo off
setlocal
cd /d "%~dp0"
set "ORCASLICER_AI_SIDECAR_PORT=18765"
set "ORCASLICER_AI_SIDECAR_URL=http://127.0.0.1:%ORCASLICER_AI_SIDECAR_PORT%"
start "OrcaSlicer AI mock" "%~dp0start_ai_sidecar_mock.bat"
start "OrcaSlicer" "%~dp0build\OrcaSlicer\orca-slicer.exe"
