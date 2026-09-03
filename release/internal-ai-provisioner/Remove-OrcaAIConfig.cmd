@echo off
setlocal EnableExtensions DisableDelayedExpansion
chcp 65001 >nul
echo Removing current-user OrcaSlicer AI PRO configuration...
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Remove-OrcaAIConfig.ps1"
set "ORCA_AI_EXIT_CODE=%ERRORLEVEL%"
if not "%ORCA_AI_EXIT_CODE%"=="0" echo Removal failed. Send the complete window text and %%LOCALAPPDATA%%\OrcaSlicer\logs\ai-config-install.log to the developer.
pause
exit /b %ORCA_AI_EXIT_CODE%
