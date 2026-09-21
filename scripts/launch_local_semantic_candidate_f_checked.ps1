$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtime = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\local-semantic-validation-20260917\runtime-candidate-f'))
$fixtures = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\local-semantic-validation-20260917\fixtures'))
$data = [IO.Path]::GetFullPath((Join-Path $workspace '.tmp\local-validation-candidate-f-user-data'))
foreach ($relative in @('orca-slicer.exe', 'OrcaSlicer.dll', 'resources', 'ai\portrait_semantics\providers.json')) {
    $path = Join-Path $runtime $relative
    if (-not (Test-Path -LiteralPath $path)) { throw "Candidate f runtime is missing: $path" }
}
if (-not (Test-Path -LiteralPath $fixtures -PathType Container)) { throw "Fixture directory is missing: $fixtures" }
New-Item -ItemType Directory -Force -Path $data | Out-Null
$env:ORCA_SEMANTIC_VALIDATION = '1'
$env:ORCA_SEMANTIC_FIXTURES = $fixtures
$process = Start-Process -FilePath (Join-Path $runtime 'orca-slicer.exe') `
    -WorkingDirectory $runtime -ArgumentList @('--datadir', ('"' + $data + '"')) -WindowStyle Normal -PassThru
Start-Sleep -Milliseconds 800
if ($process.HasExited) { throw "Candidate f exited during startup: $($process.ExitCode)" }
Write-Output "Candidate f started: PID=$($process.Id)"
Write-Output "Runtime: $runtime"
Write-Output "User data: $data"
