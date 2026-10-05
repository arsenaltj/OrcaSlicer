@echo off
setlocal

set "RUNTIME_DIR=%~dp0build-figma-3d\runtime-r142"
set "ORCASLICER_AI_SIDECAR_URL="
set "ORCASLICER_AI_OUTPUT_DIR="
set "ORCASLICER_AI_DISABLE_AUTOSTART="
set "ORCASLICER_AI_SESSION_TOKEN="

if not exist "%RUNTIME_DIR%\orca-slicer.exe" (
    echo R142 runtime was not found:
    echo   %RUNTIME_DIR%
    exit /b 1
)

cd /d "%RUNTIME_DIR%"
start "OrcaSlicer Figma 3D" "orca-slicer.exe"
exit /b 0
