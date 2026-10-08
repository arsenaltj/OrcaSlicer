@echo off
setlocal
set "runtime=%~dp0..\.tmp\dev\run"
set "review_data=%~dp0..\.tmp\dev\startup-setup-review\user-data"
if not exist "%runtime%\orca-slicer.exe" (
    echo Build the development runtime with dev.ps1 -NoLaunch first.
    pause
    exit /b 1
)
if not exist "%review_data%\." mkdir "%review_data%"
if not exist "%review_data%\." exit /b 1
set "ORCASLICER_UI_REDESIGN_STARTUP_SPLASH=1"
set "ORCASLICER_UI_REDESIGN_STARTUP_REVIEW=0"
set "ORCASLICER_UI_REDESIGN_STARTUP_BUFFER=1"
set "ORCASLICER_UI_REDESIGN_STARTUP_BUFFER_REVIEW=0"
set "ORCASLICER_UI_REDESIGN_STARTUP_SETUP=1"
pushd "%runtime%"
"%runtime%\orca-slicer.exe" --datadir "%review_data%" %*
popd
endlocal
