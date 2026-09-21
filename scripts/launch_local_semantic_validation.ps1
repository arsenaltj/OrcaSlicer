param(
    [string]$Runtime = (Join-Path $PSScriptRoot '..\artifacts\local-semantic-validation-20260917\runtime-v2'),
    [string]$Fixtures = (Join-Path $PSScriptRoot '..\artifacts\local-semantic-validation-20260917\fixtures'),
    [string]$DataDirectory = (Join-Path $PSScriptRoot '..\.tmp\local-validation-user-data')
)
$ErrorActionPreference = 'Stop'
$Runtime = [IO.Path]::GetFullPath($Runtime)
$Fixtures = [IO.Path]::GetFullPath($Fixtures)
$DataDirectory = [IO.Path]::GetFullPath($DataDirectory)
foreach ($relative in @('orca-slicer.exe','OrcaSlicer.dll','resources','ai\portrait_semantics\providers.json')) {
    if (!(Test-Path -LiteralPath (Join-Path $Runtime $relative))) { throw "Incomplete local runtime: $relative" }
}
New-Item -ItemType Directory -Path $DataDirectory -Force | Out-Null
$env:ORCA_SEMANTIC_VALIDATION = '1'
$env:ORCA_SEMANTIC_FIXTURES = $Fixtures
# This host directly opens local models; it does not create a generation service.
Start-Process -FilePath (Join-Path $Runtime 'orca-slicer.exe') -WorkingDirectory $Runtime `
    -ArgumentList @('--datadir', ('"' + $DataDirectory + '"')) -WindowStyle Normal
