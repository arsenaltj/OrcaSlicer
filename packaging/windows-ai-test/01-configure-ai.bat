@echo off
setlocal
cd /d "%~dp0"

if not exist "%~dp0setup\ai-config.bat" (
    echo setup\ai-config.bat was not found. Extract the complete package first.
    pause
    exit /b 1
)

echo Opening setup\ai-config.bat in Notepad.
echo Fill OPENAI_API_KEY and TRIPO_API_KEY, save the file, then run 02-check-environment.bat.
start "OrcaSlicer AI config" notepad.exe "%~dp0setup\ai-config.bat"
exit /b 0
