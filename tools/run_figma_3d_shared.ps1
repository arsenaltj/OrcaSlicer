[CmdletBinding()]
param(
    [string] $Runtime = '',
    [string] $DataDir = '',
    [switch] $Wait
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($Runtime)) {
    $Runtime = Join-Path $repoRoot 'build-figma-3d\runtime-r142'
}
$runtimeRoot = (Resolve-Path -LiteralPath $Runtime).Path
$exePath = Join-Path $runtimeRoot 'orca-slicer.exe'
$dllPath = Join-Path $runtimeRoot 'OrcaSlicer.dll'
$pythonPath = Join-Path $runtimeRoot 'python\pythonw.exe'
$bootstrapPath = Join-Path $runtimeRoot 'resources\tools\ai\orca_ai_installed_bootstrap.py'

foreach ($requiredPath in @($exePath, $dllPath, $pythonPath, $bootstrapPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) {
        throw "Required runtime file is missing: $requiredPath"
    }
}

if ([string]::IsNullOrWhiteSpace($DataDir)) {
    # Keep the normal Orca user profile so generated_models and its model
    # history remain shared with the installed application.
    $DataDir = Join-Path $env:APPDATA 'OrcaSlicer'
}
$dataRoot = [System.IO.Path]::GetFullPath($DataDir)
if (-not (Test-Path -LiteralPath $dataRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $dataRoot -Force | Out-Null
}

$historyRoot = Join-Path $dataRoot 'generated_models'
if (-not (Test-Path -LiteralPath $historyRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $historyRoot -Force | Out-Null
}

Write-Host "Runtime: $runtimeRoot"
Write-Host "Orca data directory: $dataRoot"
Write-Host "AI model history: $historyRoot"
Write-Host 'The local AI sidecar will be started by Orca on 127.0.0.1:18764.'

# Validation shells can leave endpoint, output or autostart overrides behind.
# This launcher is explicitly for the bundled local sidecar and shared profile.
foreach ($name in @(
        'ORCASLICER_AI_SIDECAR_URL',
        'ORCASLICER_AI_OUTPUT_DIR',
        'ORCASLICER_AI_DISABLE_AUTOSTART',
        'ORCASLICER_AI_SESSION_TOKEN')) {
    Remove-Item "Env:$name" -ErrorAction SilentlyContinue
}

$arguments = @()
if ($PSBoundParameters.ContainsKey('DataDir')) {
    # An explicit profile is useful for isolated validation. The default path
    # above intentionally leaves --datadir absent and uses the normal profile.
    $arguments += "--datadir=$dataRoot"
}

if ($arguments.Count -gt 0) {
    $process = Start-Process -FilePath $exePath -WorkingDirectory $runtimeRoot -ArgumentList $arguments -PassThru
} else {
    # Start-Process rejects an empty ArgumentList; shared mode intentionally
    # starts without --datadir so Orca selects the normal user profile.
    $process = Start-Process -FilePath $exePath -WorkingDirectory $runtimeRoot -PassThru
}
Write-Host "OrcaSlicer PID: $($process.Id)"

if ($Wait) {
    $process.WaitForExit()
    exit $process.ExitCode
}
