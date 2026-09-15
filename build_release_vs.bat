@REM OrcaSlicer build script for Windows with VS auto-detect
@echo off
set WP=%CD%
set _START_TIME=%TIME%

@REM Default target architecture to the host CPU arch; override by passing
@REM "x64" or "arm64" as an argument. PROCESSOR_ARCHITEW6432 covers a 32-bit
@REM shell running on a 64-bit OS, where PROCESSOR_ARCHITECTURE reads "x86".
set arch=x64
if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set arch=ARM64
if /I "%PROCESSOR_ARCHITEW6432%"=="ARM64" set arch=ARM64
if /I "%1"=="arm64" set arch=ARM64
if /I "%2"=="arm64" set arch=ARM64
if /I "%1"=="x64" set arch=x64
if /I "%2"=="x64" set arch=x64

@REM Check for Ninja Multi-Config option (-x)
set USE_NINJA=0
for %%a in (%*) do (
    if "%%a"=="-x" set USE_NINJA=1
)

@REM Check for clang-cl option (-l). Combined with -x it also builds the deps with
@REM clang-cl; on the Visual Studio generator it applies to the slicer only, because
@REM the dependency sub-builds have no toolset to inherit and stay on MSVC.
set CLANG_ARG=
set TOOLSET_ARG=
for %%a in (%*) do (
    if "%%a"=="-l" (
        set CLANG_ARG=-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
        set TOOLSET_ARG=-T ClangCL
    )
)

@REM Check for unit-tests option ("tests")
set BUILD_TESTS=OFF
for %%a in (%*) do (
    if /I "%%a"=="tests" set BUILD_TESTS=ON
)

if "%USE_NINJA%"=="1" (
    echo Using Ninja Multi-Config generator
    set CMAKE_GENERATOR="Ninja Multi-Config"
    set VS_VERSION=Ninja
    goto :generator_ready
)

@REM Detect Visual Studio version using msbuild
echo Detecting Visual Studio version using msbuild...

@REM Try to get MSBuild version - the output format varies by VS version
set VS_MAJOR=
for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
    for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
    set MSBUILD_OUTPUT=%%i
    goto :version_found
)

@REM Alternative method for newer MSBuild versions
if "%VS_MAJOR%"=="" (
    for /f "tokens=*" %%i in ('msbuild -version 2^>^&1 ^| findstr /r "[0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*"') do (
        for /f "tokens=1 delims=." %%a in ("%%i") do set VS_MAJOR=%%a
        set MSBUILD_OUTPUT=%%i
        goto :version_found
    )
)

:version_found
echo MSBuild version detected: %MSBUILD_OUTPUT%
echo Major version: %VS_MAJOR%

if "%VS_MAJOR%"=="" (
    echo Error: Could not determine Visual Studio version from msbuild
    echo Please ensure Visual Studio and MSBuild are properly installed
    exit /b 1
)

if "%VS_MAJOR%"=="16" (
    set VS_VERSION=2019
    set CMAKE_GENERATOR="Visual Studio 16 2019"
) else if "%VS_MAJOR%"=="17" (
    set VS_VERSION=2022
    set CMAKE_GENERATOR="Visual Studio 17 2022"
) else if "%VS_MAJOR%"=="18" (
    set VS_VERSION=2026
    set CMAKE_GENERATOR="Visual Studio 18 2026"
) else (
    echo Error: Unsupported Visual Studio version: %VS_MAJOR%
    echo Supported versions: VS2019 (16.x^), VS2022 (17.x^), VS2026 (18.x^)
    exit /b 1
)

echo Detected Visual Studio %VS_VERSION% (version %VS_MAJOR%)
echo Using CMake generator: %CMAKE_GENERATOR%

:generator_ready

@REM Pack deps
if "%1"=="pack" (
    setlocal ENABLEDELAYEDEXPANSION
    cd %WP%/deps/build
    if "%arch%"=="ARM64" cd %WP%/deps/build-arm64
    for /f "tokens=2-4 delims=/ " %%a in ('date /t') do set build_date=%%c%%b%%a
    echo packing deps: OrcaSlicer_dep_win-!arch!_!build_date!_vs!VS_VERSION!.zip

    %WP%/tools/7z.exe a OrcaSlicer_dep_win-!arch!_!build_date!_vs!VS_VERSION!.zip OrcaSlicer_dep
    goto :done
)

set debug=OFF
set debuginfo=OFF
if "%1"=="debug" set debug=ON
if "%2"=="debug" set debug=ON
if "%1"=="debuginfo" set debuginfo=ON
if "%2"=="debuginfo" set debuginfo=ON
if "%debug%"=="ON" (
    set build_type=Debug
    set build_dir=build-dbg
) else (
    if "%debuginfo%"=="ON" (
        set build_type=RelWithDebInfo
        set build_dir=build-dbginfo
    ) else (
        set build_type=Release
        set build_dir=build
    )
)
if "%arch%"=="ARM64" set build_dir=%build_dir%-arm64
echo build type set to %build_type%, arch=%arch%

setlocal DISABLEDELAYEDEXPANSION
cd deps
mkdir %build_dir%
cd %build_dir%
set "SIG_FLAG="
if defined ORCA_UPDATER_SIG_KEY set "SIG_FLAG=-DORCA_UPDATER_SIG_KEY=%ORCA_UPDATER_SIG_KEY%"
REM Always pass the complete AI cache tuple. Reusing build/ after an AI build
REM must not silently leave the installer enabled or retain an old defaults file.
set "AI_FLAGS=-DORCA_AI_WINDOWS_INSTALLER=OFF -DORCA_AI_DISTRIBUTION_CHANNEL=internal -DORCA_AI_PACKAGE_REVISION=non-ai"
set "AI_DEFAULTS_FLAG=-DORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH="
if /I not "%ORCA_AI_WINDOWS_INSTALLER%"=="ON" goto :ai_flags_done
if "%ORCA_AI_PACKAGE_REVISION%"=="" goto :ai_revision_missing
set "AI_CHANNEL=%ORCA_AI_DISTRIBUTION_CHANNEL%"
if not defined AI_CHANNEL set "AI_CHANNEL=internal"
set "AI_FLAGS=-DORCA_AI_WINDOWS_INSTALLER=ON -DORCA_AI_DISTRIBUTION_CHANNEL=%AI_CHANNEL% -DORCA_AI_PACKAGE_REVISION=%ORCA_AI_PACKAGE_REVISION%"
if /I not "%AI_CHANNEL%"=="internal" goto :ai_flags_done
if not defined ORCA_AI_INTERNAL_DEFAULTS_FILE goto :ai_flags_done
set "AI_DEFAULTS_FLAG=-DORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH=%ORCA_AI_INTERNAL_DEFAULTS_FILE%"
goto :ai_flags_done

:ai_revision_missing
echo Error: ORCA_AI_PACKAGE_REVISION is required when ORCA_AI_WINDOWS_INSTALLER=ON
exit /b 1

:ai_flags_done

@REM Metrics are opt-in and never contain CMake arguments or environment dumps.
set "ORCA_CI_METRICS_GENERATOR=%CMAKE_GENERATOR:"=%"
set "ORCA_CI_METRICS_CONFIGURATION=%build_type%"
set "ORCA_CI_METRICS_ARCHITECTURE=%arch%"
set "ORCA_CI_MSBUILD_LOG_FLAG="

if "%1"=="slicer" (
    GOTO :slicer
)
echo "building deps.."
if defined CLANG_ARG if "%USE_NINJA%"=="0" echo Note: -l needs -x for the dependencies; building them with MSVC.

echo on
REM Set minimum CMake policy to avoid <3.5 errors
set CMAKE_POLICY_VERSION_MINIMUM=3.5
@call :ci_stage_start deps-configure
@if not "%errorlevel%"=="0" goto :build_failed
if "%USE_NINJA%"=="1" (
    cmake ../ -G %CMAKE_GENERATOR% %CLANG_ARG% -DCMAKE_BUILD_TYPE=%build_type%
) else (
    cmake ../ -G %CMAKE_GENERATOR% -A %arch% -DCMAKE_BUILD_TYPE=%build_type%
)
@call :ci_stage_end deps-configure %errorlevel%
@if not "%errorlevel%"=="0" goto :build_failed
@call :ci_stage_start deps-build
@if not "%errorlevel%"=="0" goto :build_failed
if "%USE_NINJA%"=="1" (
    cmake --build . --config %build_type% --target deps
) else (
    cmake --build . --config %build_type% --target deps -- -m %ORCA_CI_MSBUILD_LOG_FLAG%
)
@call :ci_stage_end deps-build %errorlevel%
@if not "%errorlevel%"=="0" goto :build_failed
@echo off

if "%1"=="deps" goto :done

:slicer
echo "building Orca Slicer..."
cd /d "%WP%"
mkdir %build_dir%
cd %build_dir%

echo on
set CMAKE_POLICY_VERSION_MINIMUM=3.5
@call :ci_stage_start configure
@if not "%errorlevel%"=="0" goto :build_failed
if "%USE_NINJA%"=="1" (
    cmake .. -G %CMAKE_GENERATOR% %CLANG_ARG% -DORCA_TOOLS=ON %SIG_FLAG% %AI_FLAGS% "%AI_DEFAULTS_FLAG%" -DBUILD_TESTS=%BUILD_TESTS% -DCMAKE_BUILD_TYPE=%build_type%
) else (
    cmake .. -G %CMAKE_GENERATOR% -A %arch% %TOOLSET_ARG% -DORCA_TOOLS=ON %SIG_FLAG% %AI_FLAGS% "%AI_DEFAULTS_FLAG%" -DBUILD_TESTS=%BUILD_TESTS% -DCMAKE_BUILD_TYPE=%build_type%
)
@call :ci_stage_end configure %errorlevel%
@if not "%errorlevel%"=="0" goto :build_failed
@call :ci_stage_start build
@if not "%errorlevel%"=="0" goto :build_failed
if "%USE_NINJA%"=="1" (
    cmake --build . --config %build_type% --target all
) else (
    cmake --build . --config %build_type% --target ALL_BUILD -- -m %ORCA_CI_MSBUILD_LOG_FLAG%
)
@call :ci_stage_end build %errorlevel%
@if not "%errorlevel%"=="0" goto :build_failed
@echo off
cd ..
call :ci_stage_start gettext
if not "%errorlevel%"=="0" goto :build_failed
call scripts/run_gettext.bat
call :ci_stage_end gettext %errorlevel%
if not "%errorlevel%"=="0" goto :build_failed
cd %build_dir%
call :ci_stage_start install
if not "%errorlevel%"=="0" goto :build_failed
if defined ORCA_CI_MSBUILD_LOG_FLAG (
    cmake --build . --target install --config %build_type% -- %ORCA_CI_MSBUILD_LOG_FLAG%
) else (
    cmake --build . --target install --config %build_type%
)
call :ci_stage_end install %errorlevel%
if not "%errorlevel%"=="0" goto :build_failed

:done
@echo off
for /f "tokens=1-3 delims=:.," %%a in ("%_START_TIME: =0%") do set /a "_start_s=(1%%a-100)*3600+(1%%b-100)*60+1%%c-100"
for /f "tokens=1-3 delims=:.," %%a in ("%TIME: =0%") do set /a "_end_s=(1%%a-100)*3600+(1%%b-100)*60+1%%c-100"
set /a "_elapsed=_end_s - _start_s"
if %_elapsed% lss 0 set /a "_elapsed+=86400"
set /a "_hours=_elapsed / 3600"
set /a "_remainder=_elapsed - _hours * 3600"
set /a "_mins=_remainder / 60"
set /a "_secs=_remainder - _mins * 60"
echo.
echo Build completed in %_hours%h %_mins%m %_secs%s
exit /b 0

:build_failed
set "_ORCA_BUILD_EXIT=%errorlevel%"
@echo off
echo Build failed with exit code %_ORCA_BUILD_EXIT%.
exit /b %_ORCA_BUILD_EXIT%

:ci_stage_start
set "ORCA_CI_MSBUILD_LOG_FLAG="
if not defined ORCA_CI_METRICS_PATH exit /b 0
set "ORCA_CI_METRICS_BUILD_DIR=%CD%"
set "ORCA_CI_METRICS_BUILD_LOG="
set "ORCA_CI_METRICS_BUILD_LOG_KIND="
if "%~1"=="deps-configure" goto :ci_stage_start_record
if "%~1"=="configure" goto :ci_stage_start_record
if "%~1"=="gettext" goto :ci_stage_start_record
if "%USE_NINJA%"=="1" (
    set "ORCA_CI_METRICS_BUILD_LOG=%CD%\.ninja_log"
    set "ORCA_CI_METRICS_BUILD_LOG_KIND=ninja_log"
    goto :ci_stage_start_record
)
if not "%ORCA_CI_BUILD_LOGS%"=="1" goto :ci_stage_start_record
for %%I in ("%ORCA_CI_METRICS_PATH%") do set "ORCA_CI_METRICS_BUILD_LOG=%%~dpI%~1.binlog"
set "ORCA_CI_METRICS_BUILD_LOG_KIND=msbuild_binlog"
set ORCA_CI_MSBUILD_LOG_FLAG=/bl:"%ORCA_CI_METRICS_BUILD_LOG%";ProjectImports=None
:ci_stage_start_record
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%WP%\scripts\ci_windows_build_metrics.ps1" -Action Start -Stage "%~1"
exit /b %errorlevel%

:ci_stage_end
set "_ORCA_NATIVE_EXIT=%~2"
if not defined ORCA_CI_METRICS_PATH exit /b %_ORCA_NATIVE_EXIT%
powershell.exe -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File "%WP%\scripts\ci_windows_build_metrics.ps1" -Action Complete -Stage "%~1" -ExitCode %_ORCA_NATIVE_EXIT%
set "_ORCA_METRICS_EXIT=%errorlevel%"
if not "%_ORCA_NATIVE_EXIT%"=="0" exit /b %_ORCA_NATIVE_EXIT%
exit /b %_ORCA_METRICS_EXIT%
