@echo off
setlocal
cd /d "%~dp0\..\.."

if "%ORCASLICER_AI_SIDECAR_PORT%"=="" set "ORCASLICER_AI_SIDECAR_PORT=18764"
if "%OPENAI_BASE_URL%"=="" set "OPENAI_BASE_URL=https://api.openai.com/v1"
if "%OPENAI_TEXT_MODEL%"=="" set "OPENAI_TEXT_MODEL=gpt-5.4"
if "%OPENAI_IMAGE_MODEL%"=="" set "OPENAI_IMAGE_MODEL=gpt-image-2"
if "%TRIPO_API_BASE%"=="" set "TRIPO_API_BASE=https://openapi.tripo3d.com/v3"
if "%TRIPO_MODEL%"=="" set "TRIPO_MODEL=v3.1-20260211"

if "%PYTHON_EXE%"=="" set "PYTHON_EXE=python"
"%PYTHON_EXE%" --version >nul 2>nul
if errorlevel 1 (
    echo Python was not found. Install Python 3 or set PYTHON_EXE.
    exit /b 1
)

echo Starting OrcaSlicer AI sidecar on port %ORCASLICER_AI_SIDECAR_PORT%.
if "%OPENAI_API_KEY%"=="" echo AI text and image features are not configured.
if "%TRIPO_API_KEY%"=="" echo 3D model generation is not configured.

"%PYTHON_EXE%" tools\ai\orca_ai_sidecar.py
