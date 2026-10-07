[CmdletBinding()]
param(
    [string]$DataDir,
    [string]$RuntimeDir,
    [ValidateRange(1024, 65535)][int]$Port = 18767
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workflowRoot = Split-Path $PSScriptRoot -Parent
$runtime = if ($RuntimeDir) { [IO.Path]::GetFullPath($RuntimeDir) } else { Join-Path $workflowRoot '.tmp/dev/run' }
if (-not $DataDir) { $DataDir = Join-Path $workflowRoot '.tmp/dev/workflow-review' }
$DataDir = [IO.Path]::GetFullPath($DataDir)
$application = Join-Path $runtime 'orca-slicer.exe'
$python = Join-Path $runtime 'python/pythonw.exe'
$bootstrap = Join-Path $runtime 'resources/tools/ai/orca_ai_installed_bootstrap.py'
foreach ($file in @($application, $python, $bootstrap)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Build dev.ps1 Run -NoLaunch first. Missing: $file" }
}
$listeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
if (@($listeners | Where-Object Port -eq $Port).Count) { throw "Review port $Port is already in use; use -Port with a free port." }
New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
$reviewTemp = Join-Path $DataDir 'temp'
New-Item -ItemType Directory -Path $reviewTemp -Force | Out-Null
$environment = @{
    TEMP = $reviewTemp
    TMP = $reviewTemp
    ORCASLICER_MODEL_WORKFLOW_REVIEW = '1'
    ORCASLICER_UI_REDESIGN_STARTUP_SPLASH = '1'
    ORCASLICER_UI_REDESIGN_STARTUP_REVIEW = '0'
    ORCASLICER_UI_REDESIGN_STARTUP_BUFFER = '1'
    ORCASLICER_UI_REDESIGN_STARTUP_BUFFER_REVIEW = '0'
    ORCASLICER_UI_REDESIGN_STARTUP_SETUP = '1'
    ORCASLICER_AI_SIDECAR_URL = "http://127.0.0.1:$Port"
    ORCASLICER_AI_SIDECAR_HOST = '127.0.0.1'
    ORCASLICER_AI_SIDECAR_PORT = [string]$Port
    ORCASLICER_AI_OUTPUT_DIR = (Join-Path $DataDir 'generated_models')
    ORCASLICER_AI_SESSION_TOKEN = ([Guid]::NewGuid().ToString('N') + [Guid]::NewGuid().ToString('N'))
    ORCASLICER_AI_PARENT_PID = ''
}
$savedEnvironment = @{}
try {
    foreach ($name in $environment.Keys) {
        $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
        [Environment]::SetEnvironmentVariable($name, $environment[$name], 'Process')
    }
    $app = Start-Process -FilePath $application -ArgumentList @('--datadir', ('"' + $DataDir + '"')) -WorkingDirectory $runtime -WindowStyle Normal -PassThru
    if ($app.WaitForExit(1500)) { throw "Review application exited during startup (exit $($app.ExitCode)). See $DataDir/log." }
    [Environment]::SetEnvironmentVariable('ORCASLICER_AI_PARENT_PID', [string]$app.Id, 'Process')
    $sidecar = Start-Process -FilePath $python -ArgumentList @('-I', ('"' + $bootstrap + '"'), ('"' + $DataDir + '"')) -WorkingDirectory $runtime -WindowStyle Hidden -PassThru
    if ($sidecar.WaitForExit(1500)) { throw "Review AI service exited during startup (exit $($sidecar.ExitCode)). See $DataDir/log." }
    Write-Host "Runtime: $runtime"
    Write-Host "Review data: $DataDir"
    Write-Host "Review AI service: http://127.0.0.1:$Port"
    Write-Host "Orca PID: $($app.Id); AI service PID: $($sidecar.Id)"
} finally {
    foreach ($name in $savedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name], 'Process')
    }
}
