@echo off
setlocal EnableExtensions

set "PACKAGE_ZIP=%~dp0OrcaAI-demo3.zip"
set "DESTINATION=%USERPROFILE%\OrcaAI-demo3"
if defined ORCA_AI_EXTRACT_DEST set "DESTINATION=%ORCA_AI_EXTRACT_DEST%"

if not exist "%PACKAGE_ZIP%" (
    echo OrcaAI-demo3.zip was not found next to this script.
    echo Keep the ZIP and this BAT file in the same folder.
    if not defined ORCA_AI_EXTRACT_NONINTERACTIVE pause
    exit /b 1
)

if exist "%DESTINATION%" (
    echo Destination already exists:
    echo %DESTINATION%
    echo Rename or remove that folder before extracting again.
    if not defined ORCA_AI_EXTRACT_NONINTERACTIVE pause
    exit /b 1
)

mkdir "%DESTINATION%" || (
    echo Could not create %DESTINATION%.
    if not defined ORCA_AI_EXTRACT_NONINTERACTIVE pause
    exit /b 1
)

echo Extracting 15,000+ OrcaSlicer resource files.
echo Please wait. This normally takes about one minute.
"%SystemRoot%\System32\tar.exe" -xf "%PACKAGE_ZIP%" -C "%DESTINATION%"
if errorlevel 1 (
    echo Extraction failed. The incomplete folder is:
    echo %DESTINATION%
    if not defined ORCA_AI_EXTRACT_NONINTERACTIVE pause
    exit /b 1
)

if not exist "%DESTINATION%\03-start-orcaslicer-ai.bat" (
    echo Extraction finished but the package entry file is missing.
    if not defined ORCA_AI_EXTRACT_NONINTERACTIVE pause
    exit /b 1
)

echo.
echo Extraction completed:
echo %DESTINATION%
if not defined ORCA_AI_EXTRACT_NONINTERACTIVE start "OrcaSlicer AI package" explorer.exe "%DESTINATION%"
if not defined ORCA_AI_EXTRACT_NONINTERACTIVE pause
exit /b 0
