param(
    [string]$ExePath = "$PSScriptRoot\..\build-pr15\src\Release\orca-slicer.exe",
    [string]$SourceDataDir = "$env:APPDATA\OrcaSlicer",
    [string]$RunRoot = "$PSScriptRoot\..\.tmp\readonly-evidence-ui",
    [string]$HistoryId = "",
    [switch]$NoLaunch
)

$ErrorActionPreference = "Stop"
$sourceModels = Join-Path $SourceDataDir "generated_models"
if (-not (Test-Path -LiteralPath $sourceModels -PathType Container)) {
    throw "History model directory does not exist: $sourceModels"
}

if ([string]::IsNullOrWhiteSpace($HistoryId)) {
    $entry = Get-ChildItem -LiteralPath $sourceModels -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "model.glb") -PathType Leaf } |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $entry) { throw "No history entry containing model.glb was found" }
    $HistoryId = $entry.Name
} else {
    $entry = Get-Item -LiteralPath (Join-Path $sourceModels $HistoryId) -ErrorAction Stop
    if (-not (Test-Path -LiteralPath (Join-Path $entry.FullName "model.glb") -PathType Leaf)) {
        throw "History entry does not contain model.glb: $HistoryId"
    }
}

$runPath = [System.IO.Path]::GetFullPath((Join-Path $RunRoot ("history-" + $HistoryId)))
if (Test-Path -LiteralPath $runPath) {
    throw "Test directory already exists; choose another RunRoot: $runPath"
}
$destinationDownloads = Join-Path $runPath "generated_models\downloads"
New-Item -ItemType Directory -Path $destinationDownloads -Force | Out-Null
$sourceDownloads = Join-Path $sourceModels "downloads"
$downloadFiles = Get-ChildItem -LiteralPath $sourceDownloads -Filter ("orcaslicer-ai-" + $HistoryId + "*") -File
if (-not $downloadFiles) { throw "No downloadable history assets were found for: $HistoryId" }
Copy-Item -LiteralPath $downloadFiles.FullName -Destination $destinationDownloads -Force

Write-Output "History model: $HistoryId"
Write-Output "Test data directory: $runPath"
Write-Output "Model copy: $(Join-Path $destinationDownloads ("orcaslicer-ai-" + $HistoryId + ".glb"))"

if (-not $NoLaunch) {
    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) {
        throw "Test executable was not found: $ExePath"
    }
    Start-Process -FilePath ([System.IO.Path]::GetFullPath($ExePath)) -ArgumentList ("--datadir=" + $runPath) -WorkingDirectory (Split-Path -Parent $ExePath)
    Write-Output "Test executable started: $ExePath"
}
