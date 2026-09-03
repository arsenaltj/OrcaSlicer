@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
echo Installing OrcaSlicer AI PRO configuration for the current Windows user...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Install-OrcaAIConfig.ps1"
set "ORCA_AI_EXIT_CODE=%ERRORLEVEL%"
if not "%ORCA_AI_EXIT_CODE%"=="0" echo Installation failed. Send the complete window text and %%LOCALAPPDATA%%\OrcaSlicer\logs\ai-config-install.log to the developer.
pause
exit /b %ORCA_AI_EXIT_CODE%
