@echo off
setlocal
cd /d "%~dp0"
call "%~dp0setup\ai-config.bat"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup\Check-Environment.ps1"
if errorlevel 1 goto failed
call "%~dp003-start-orcaslicer-ai.bat" --check
if errorlevel 1 goto failed
echo.
echo Environment and local AI service checks passed.
echo This check did not create a paid image or 3D task.
pause
exit /b 0

:failed
echo.
echo Environment check failed. Fill setup\ai-config.bat and try again.
pause
exit /b 1
