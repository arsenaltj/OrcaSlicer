@echo off
setlocal
cd /d "%~dp0"
if "%ORCASLICER_AI_SIDECAR_PORT%"=="" set "ORCASLICER_AI_SIDECAR_PORT=18765"
python tools\ai_sidecar_mock.py
