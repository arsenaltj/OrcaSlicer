$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$runtime = [IO.Path]::GetFullPath((Join-Path $workspace 'build-validation\src\Release'))
$fixtures = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\local-semantic-validation-20260917\fixtures'))
$data = [IO.Path]::GetFullPath((Join-Path $workspace '.tmp\local-semantic-v24-user-data'))

foreach ($relative in @(
    'orca-slicer.exe',
    'OrcaSlicer.dll',
    'resources',
    'ai\portrait_semantics\providers.json',
    'ai\portrait_semantics\boundary\onnxruntime.dll'
)) {
    $path = Join-Path $runtime $relative
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Current v24 runtime is missing: $path"
    }
}
if (-not (Test-Path -LiteralPath $fixtures -PathType Container)) {
    throw "Semantic validation fixtures are missing: $fixtures"
}

New-Item -ItemType Directory -Force -Path $data | Out-Null
$env:ORCA_SEMANTIC_VALIDATION = '1'
$env:ORCA_SEMANTIC_FIXTURES = $fixtures
$process = Start-Process -FilePath (Join-Path $runtime 'orca-slicer.exe') `
    -WorkingDirectory $runtime `
    -ArgumentList @('--datadir', ('"' + $data + '"')) `
    -WindowStyle Normal `
    -PassThru

Start-Sleep -Milliseconds 1200
if ($process.HasExited) {
    throw "Current v24 build exited during startup: $($process.ExitCode)"
}

Write-Output "Current v24 build started: PID=$($process.Id)"
Write-Output "Runtime: $runtime"
Write-Output "User data: $data"
