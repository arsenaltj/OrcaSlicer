@echo off
setlocal
cd /d "%~dp0"

call "%~dp0tools\ai\refresh_ai_environment.bat"
call "%~dp0setup\ai-config.bat"

set "ORCASLICER_AI_SIDECAR_PORT=18764"
set "ORCASLICER_AI_SIDECAR_URL=http://127.0.0.1:%ORCASLICER_AI_SIDECAR_PORT%"
set "ORCASLICER_AI_OUTPUT_DIR=%~dp0generated_models"
set "ORCASLICER_AI_CHECK_ONLY=0"
if /i "%~1"=="--check" set "ORCASLICER_AI_CHECK_ONLY=1"

if not exist "%~dp0OrcaSlicer\orca-slicer.exe" (
    echo OrcaSlicer\orca-slicer.exe was not found. Extract the complete package first.
    exit /b 1
)

call :check_ai_sidecar
set "AI_SIDECAR_STATUS=%ERRORLEVEL%"
if "%AI_SIDECAR_STATUS%"=="0" goto ai_sidecar_ready
if "%AI_SIDECAR_STATUS%"=="2" goto ai_sidecar_unavailable
if "%AI_SIDECAR_STATUS%"=="3" goto ai_sidecar_configuration_changed

:start_ai_sidecar
echo Starting OrcaSlicer AI sidecar.
start "OrcaSlicer AI sidecar" /min "%~dp0tools\ai\start_orca_ai_sidecar.bat"

set /a AI_SIDECAR_ATTEMPT=0
:wait_for_ai_sidecar
call :check_ai_sidecar
set "AI_SIDECAR_STATUS=%ERRORLEVEL%"
if "%AI_SIDECAR_STATUS%"=="0" goto ai_sidecar_ready
if "%AI_SIDECAR_STATUS%"=="2" goto ai_sidecar_unavailable
set /a AI_SIDECAR_ATTEMPT+=1
if %AI_SIDECAR_ATTEMPT% GEQ 30 goto ai_sidecar_timeout
timeout /t 1 /nobreak >nul
goto wait_for_ai_sidecar

:ai_sidecar_configuration_changed
echo AI provider configuration changed; restarting the formal sidecar.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ai\stop_orca_ai_sidecar.ps1" -Endpoint "%ORCASLICER_AI_SIDECAR_URL%"
if errorlevel 1 (
    echo The process on port %ORCASLICER_AI_SIDECAR_PORT% is not the formal OrcaSlicer AI sidecar.
    exit /b 1
)
goto start_ai_sidecar

:ai_sidecar_ready
echo AI sidecar is ready for real text, image and OBJ generation.
if "%ORCASLICER_AI_CHECK_ONLY%"=="1" exit /b 0
start "OrcaSlicer" "%~dp0OrcaSlicer\orca-slicer.exe"
exit /b 0

:ai_sidecar_unavailable
echo AI sidecar is running, but real model generation is unavailable.
echo Fill setup\ai-config.bat and verify the API settings.
exit /b 2

:ai_sidecar_timeout
echo AI sidecar did not become ready within 30 seconds.
exit /b 1

:check_ai_sidecar
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ai\check_sidecar_capability.ps1" -Endpoint "%ORCASLICER_AI_SIDECAR_URL%" -ExpectedOpenAIBaseUrl "%OPENAI_BASE_URL%" -ExpectedSidecarVersion "orcaslicer-ai-sidecar-v5" >nul 2>nul
exit /b %ERRORLEVEL%
