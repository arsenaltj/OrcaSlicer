@echo off
rem Import persistent Windows settings into the current cmd.exe process.
rem Machine values are loaded first; user values take precedence.
for %%V in (OPENAI_BASE_URL OPENAI_API_KEY OPENAI_TEXT_MODEL OPENAI_IMAGE_MODEL TRIPO_API_KEY TRIPO_API_BASE TRIPO_MODEL PYTHON_EXE ORCASLICER_AI_ALLOW_PREPROCESS_FALLBACK) do call :refresh_windows_environment %%V
exit /b 0

:refresh_windows_environment
set "ORCA_AI_ENV_VALUE="
for /f "tokens=2,*" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Environment" /v "%~1" 2^>nul') do set "ORCA_AI_ENV_VALUE=%%B"
for /f "tokens=2,*" %%A in ('reg query "HKCU\Environment" /v "%~1" 2^>nul') do set "ORCA_AI_ENV_VALUE=%%B"
if defined ORCA_AI_ENV_VALUE set "%~1=%ORCA_AI_ENV_VALUE%"
exit /b 0
