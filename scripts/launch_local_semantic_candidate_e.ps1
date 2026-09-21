$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
& (Join-Path $PSScriptRoot 'launch_local_semantic_validation.ps1') `
    -Runtime (Join-Path $workspace 'artifacts\local-semantic-validation-20260917\runtime-candidate-e') `
    -Fixtures (Join-Path $workspace 'artifacts\local-semantic-validation-20260917\fixtures') `
    -DataDirectory (Join-Path $workspace '.tmp\local-validation-candidate-e-user-data')
