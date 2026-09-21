$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$checked = Join-Path $PSScriptRoot 'launch_local_semantic_candidate_f_checked.ps1'
& powershell.exe -NoProfile -ExecutionPolicy Bypass -File $checked
exit $LASTEXITCODE
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))

$runtime = [IO.Path]::GetFullPath((Join-Path $workspace 'artifacts\local-semantic-validation-20260917\runtime-candidate-f'))
$required = @('orca-slicer.exe', 'OrcaSlicer.dll', 'resources', 'ai\portrait_semantics\providers.json')
foreach ($relative in $required) {
    $path = Join-Path $runtime $relative
    if (-not (Test-Path -LiteralPath $path)) { throw "Candidate f runtime is missing: $path" }
}
& (Join-Path $PSScriptRoot 'launch_local_semantic_validation.ps1') `
    -Runtime (Join-Path $workspace 'artifacts\local-semantic-validation-20260917\runtime-candidate-f') `
    -Fixtures (Join-Path $workspace 'artifacts\local-semantic-validation-20260917\fixtures') `
    -DataDirectory (Join-Path $workspace '.tmp\local-validation-candidate-f-user-data')
