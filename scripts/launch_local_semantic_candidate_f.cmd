@echo off
setlocal
set "SCRIPT_DIR=%~dp0"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%launch_local_semantic_candidate_f_checked.ps1"
if errorlevel 1 (
  echo.
  echo Candidate f failed to start. Review the error above.
  pause
  exit /b 1
)
echo.
echo Candidate f is running. This window can be closed.
pause
