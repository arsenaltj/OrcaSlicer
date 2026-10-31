@echo off
setlocal
for /f "usebackq delims=" %%n in (`powershell -NoProfile -Command "(Get-Content -LiteralPath '%~dp0..\config\product.json' -Raw | ConvertFrom-Json).display_name"`) do set "product_name=%%n"
if not defined product_name exit /b 1
set "runtime=%~dp0..\.tmp\dev\run"
set "review_data=%~dp0..\.tmp\dev\startup-buffer-review\data"
if not exist "%runtime%\%product_name%.exe" (
    echo Build the development runtime with dev.ps1 -NoLaunch first.
    pause
    exit /b 1
)
if not exist "%review_data%\." mkdir "%review_data%"
if not exist "%review_data%\." (
    echo Could not create the isolated review data directory.
    pause
    exit /b 1
)
set "ORCASLICER_UI_REDESIGN_STARTUP_SPLASH=1"
set "ORCASLICER_UI_REDESIGN_STARTUP_REVIEW=0"
set "ORCASLICER_UI_REDESIGN_STARTUP_BUFFER=1"
set "ORCASLICER_UI_REDESIGN_STARTUP_BUFFER_REVIEW=1"
echo Startup buffer review: Escape or the window close icon exits.
pushd "%runtime%"
"%runtime%\%product_name%.exe" --datadir "%review_data%"
popd
endlocal
