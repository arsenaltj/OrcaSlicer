param(
    [Parameter(Mandatory)][string]$SourceDir,
    [Parameter(Mandatory)][string]$InstallDir,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64'
)

$ErrorActionPreference = 'Stop'
$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsInstall = & $vswherePath -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsInstall) { throw 'Visual Studio C++ Build Tools are required for FFmpeg.' }

$makePath = $env:ORCA_FFMPEG_MAKE
if (!$makePath) { $makePath = (Get-Command make.exe -ErrorAction SilentlyContinue).Source }
if (!$makePath) { $makePath = 'C:/msys64/usr/bin/make.exe' }
if (!(Test-Path -LiteralPath $makePath) -or !((& $makePath --version) -match '^Built for .*msys')) {
    throw 'Install MSYS2 GNU Make and set ORCA_FFMPEG_MAKE to its usr/bin/make.exe.'
}
$bashPath = Join-Path (Split-Path -Parent $makePath) 'bash.exe'
if (!(Test-Path -LiteralPath $bashPath)) {
    $gitPath = (Get-Command git.exe -ErrorAction Stop).Source
    $gitRoot = Split-Path -Parent (Split-Path -Parent $gitPath)
    $bashPath = Join-Path $gitRoot 'bin/bash.exe'
}

$vcvarsPath = Join-Path $vsInstall 'VC/Auxiliary/Build/vcvarsall.bat'
$vcvarsArchitecture = if ($Architecture -eq 'arm64') { 'x64_arm64' } else { 'x64' }
$compilerEnvironment = & cmd.exe /d /c "call `"$vcvarsPath`" $vcvarsArchitecture >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Could not initialize the Visual Studio toolchain for FFmpeg.' }
foreach ($entry in $compilerEnvironment) {
    if ($entry -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($matches[1], $matches[2], 'Process') }
}

$ffmpegJobs = if ($env:CMAKE_BUILD_PARALLEL_LEVEL -match '^[1-9][0-9]*$') { $env:CMAKE_BUILD_PARALLEL_LEVEL } else { '4' }
$ffmpegArch = if ($Architecture -eq 'arm64') { 'aarch64' } else { 'x86_64' }
$env:FFMPEG_MSVC_BIN = Join-Path $env:VCToolsInstallDir "bin/Hostx64/$Architecture"
$env:VSLANG = '1033'
$buildScript = Join-Path $PSScriptRoot 'build_windows.sh'
& $bashPath $buildScript $SourceDir $InstallDir $makePath $ffmpegArch $ffmpegJobs
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg build or installation failed.' }
Get-ChildItem -LiteralPath (Join-Path $InstallDir 'bin') -Filter '*.lib' -File |
    Copy-Item -Destination (Join-Path $InstallDir 'lib') -Force
