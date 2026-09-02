@echo off
setlocal
cd /d "%~dp0"

call "%~dp0tools\ai\refresh_ai_environment.bat"

set "ORCASLICER_AI_SIDECAR_PORT=18764"
set "ORCASLICER_AI_SIDECAR_URL=http://127.0.0.1:%ORCASLICER_AI_SIDECAR_PORT%"
set "ORCASLICER_AI_SIDECAR_VERSION=orcaslicer-ai-sidecar-v9"
set "ORCASLICER_AI_OUTPUT_DIR=%~dp0generated_models"
set "ORCASLICER_RUNTIME_EXE=%~dp0build\OrcaSlicer\orca-slicer.exe"
set "ORCASLICER_RUNTIME_DLL=%~dp0build\OrcaSlicer\OrcaSlicer.dll"
set "ORCASLICER_RELEASE_DLL=%~dp0build\src\Release\OrcaSlicer.dll"
set "ORCASLICER_AI_CHECK_ONLY=0"
if /i "%~1"=="--check" set "ORCASLICER_AI_CHECK_ONLY=1"

if not exist "%ORCASLICER_RUNTIME_EXE%" (
    echo OrcaSlicer was not found at build\OrcaSlicer\orca-slicer.exe.
    echo Build and install the Release configuration first.
    exit /b 1
)
if not exist "%ORCASLICER_RELEASE_DLL%" (
    echo The latest Release DLL was not found at build\src\Release\OrcaSlicer.dll.
    echo Build the Release OrcaSlicer target first.
    exit /b 1
)

if exist "%ORCASLICER_RUNTIME_DLL%" (
    fc /b "%ORCASLICER_RELEASE_DLL%" "%ORCASLICER_RUNTIME_DLL%" >nul
    if not errorlevel 1 goto runtime_ready
)
copy /y "%ORCASLICER_RELEASE_DLL%" "%ORCASLICER_RUNTIME_DLL%" >nul
if errorlevel 1 goto runtime_sync_failed

:runtime_ready
echo OrcaSlicer Release runtime is ready.

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
ping.exe -n 2 127.0.0.1 >nul
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
echo AI sidecar is ready for real text and image generation.
if "%ORCASLICER_AI_CHECK_ONLY%"=="1" exit /b 0
goto launch_orcaslicer

:ai_sidecar_unavailable
echo AI sidecar is running, but real model generation is unavailable.
echo Verify OPENAI_API_KEY and TRIPO_API_KEY, then restart the sidecar.
exit /b 2

:ai_sidecar_timeout
echo AI sidecar did not become ready within 30 seconds.
exit /b 1

:launch_orcaslicer
start "OrcaSlicer" "%ORCASLICER_RUNTIME_EXE%"
exit /b 0

:runtime_sync_failed
echo Failed to update build\OrcaSlicer\OrcaSlicer.dll from the latest Release build.
echo Close running OrcaSlicer windows if the runtime DLL is out of date, then try again.
exit /b 1

:check_ai_sidecar
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\ai\check_sidecar_capability.ps1" -Endpoint "%ORCASLICER_AI_SIDECAR_URL%" -ExpectedOpenAIBaseUrl "%OPENAI_BASE_URL%" -ExpectedSidecarVersion "%ORCASLICER_AI_SIDECAR_VERSION%" >nul 2>nul
exit /b %ERRORLEVEL%
