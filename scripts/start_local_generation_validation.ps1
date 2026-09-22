[CmdletBinding()]
param(
    [string] $Endpoint = 'http://127.0.0.1:18764',
    [string] $Python = 'python',
    [string] $Orca = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if ([string]::IsNullOrWhiteSpace($Orca)) {
    $Orca = Join-Path $repoRoot 'build-semantic\src\Release\orca-slicer.exe'
}
if (-not (Test-Path -LiteralPath $Orca -PathType Leaf)) {
    throw "OrcaSlicer executable was not found: $Orca"
}
if (-not (Test-Path -LiteralPath (Join-Path $repoRoot 'tools\ai\orca_ai_sidecar.py') -PathType Leaf)) {
    throw 'Local Orca AI Sidecar source was not found.'
}

$tokenBytes = [byte[]]::new(32)
[System.Security.Cryptography.RandomNumberGenerator]::Fill($tokenBytes)
$token = ([System.BitConverter]::ToString($tokenBytes) -replace '-', '').ToLowerInvariant()

$orcaInfo = [System.Diagnostics.ProcessStartInfo]::new()
$orcaInfo.FileName = $Orca
$orcaInfo.WorkingDirectory = Split-Path -Parent $Orca
$orcaInfo.UseShellExecute = $false
$orcaInfo.Environment['ORCASLICER_AI_SIDECAR_URL'] = $Endpoint
$orcaInfo.Environment['ORCASLICER_AI_SESSION_TOKEN'] = $token
$orcaInfo.Environment['ORCASLICER_AI_DISABLE_AUTOSTART'] = '1'
$orcaProcess = [System.Diagnostics.Process]::Start($orcaInfo)
if ($null -eq $orcaProcess) { throw 'Failed to start OrcaSlicer.' }

$sidecarInfo = [System.Diagnostics.ProcessStartInfo]::new()
$sidecarInfo.FileName = $Python
$sidecarInfo.Arguments = ('-I "{0}"' -f (Join-Path $repoRoot 'tools\ai\orca_ai_sidecar.py'))
$sidecarInfo.WorkingDirectory = $repoRoot
$sidecarInfo.UseShellExecute = $false
$sidecarInfo.Environment['ORCASLICER_AI_SIDECAR_URL'] = $Endpoint
$sidecarInfo.Environment['ORCASLICER_AI_SESSION_TOKEN'] = $token
$sidecarInfo.Environment['ORCASLICER_AI_REQUIRE_SESSION'] = '1'
$sidecarInfo.Environment['ORCASLICER_AI_PARENT_PID'] = [string]$orcaProcess.Id
$sidecarInfo.Environment['ORCASLICER_AI_SIDECAR_PORT'] = ([uri]$Endpoint).Port
$sidecarInfo.Environment['ORCASLICER_AI_OUTPUT_DIR'] = Join-Path $repoRoot 'generated_models'
$sidecarInfo.RedirectStandardOutput = $true
$sidecarInfo.RedirectStandardError = $true
$sidecarInfo.CreateNoWindow = $true
$sidecarProcess = [System.Diagnostics.Process]::Start($sidecarInfo)
if ($null -eq $sidecarProcess) {
    $orcaProcess.Kill()
    throw 'Failed to start local Orca AI Sidecar.'
}

Write-Host "OrcaSlicer PID: $($orcaProcess.Id)"
Write-Host "Local generation Sidecar PID: $($sidecarProcess.Id)"
Write-Host "Endpoint: $Endpoint"
Write-Host 'Close OrcaSlicer to stop the bound Sidecar automatically.'

$sidecarProcess.BeginOutputReadLine()
$sidecarProcess.BeginErrorReadLine()
while (-not $orcaProcess.HasExited) {
    Start-Sleep -Milliseconds 500
}
if (-not $sidecarProcess.HasExited) {
    $sidecarProcess.Kill()
    $sidecarProcess.WaitForExit()
}
