@echo off
setlocal ENABLEEXTENSIONS ENABLEDELAYEDEXPANSION

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "VS_VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set "VS_CMAKE=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
set "STRAWBERRY_PERL=D:\Tools\StrawberryPerl-5.10.1.1\perl\bin"

if /I "%~1"=="help" goto :help
if /I "%~1"=="-h" goto :help
if /I "%~1"=="--help" goto :help

if not exist "%VS_VCVARS%" (
    echo ERROR: Visual Studio vcvars64.bat was not found:
    echo   %VS_VCVARS%
    exit /b 1
)

if not exist "%VS_CMAKE%\cmake.exe" (
    echo ERROR: Visual Studio CMake was not found:
    echo   %VS_CMAKE%\cmake.exe
    exit /b 1
)

if not exist "%STRAWBERRY_PERL%\perl.exe" (
    echo ERROR: Strawberry Perl was not found:
    echo   %STRAWBERRY_PERL%\perl.exe
    echo Move or install Strawberry Perl there, or edit STRAWBERRY_PERL in this script.
    exit /b 1
)

set "BUILD_ARGS=%*"
if "%BUILD_ARGS%"=="" set "BUILD_ARGS=slicer x64"

echo Using repository: %ROOT%
echo Using Visual Studio: %VS_VCVARS%
echo Using CMake: %VS_CMAKE%\cmake.exe
echo Using Perl: %STRAWBERRY_PERL%\perl.exe
echo Forwarding args: %BUILD_ARGS%
echo.

call "%VS_VCVARS%"
if errorlevel 1 exit /b %errorlevel%

set "PATH=%STRAWBERRY_PERL%;%VS_CMAKE%;%PATH%"
cd /d "%ROOT%"
call "%ROOT%\build_release_vs2022.bat" %BUILD_ARGS%
exit /b %errorlevel%

:help
echo OrcaSlicer local build helper
echo.
echo Usage:
echo   build_orca_local.bat                 Build OrcaSlicer Release x64
echo   build_orca_local.bat slicer x64      Build OrcaSlicer Release x64
echo   build_orca_local.bat deps x64        Build dependencies only
echo   build_orca_local.bat x64             Build deps and slicer
echo   build_orca_local.bat slicer debuginfo x64
echo   build_orca_local.bat slicer debug x64
echo.
echo Output:
echo   build\OrcaSlicer\orca-slicer.exe
exit /b 0
