@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%launch_local_semantic_v24.ps1"
if errorlevel 1 (
  echo.
  echo Current v24 build failed to start. Review the error above.
  pause
  exit /b 1
)
echo.
echo Current v24 build is running. This window can be closed.
pause
