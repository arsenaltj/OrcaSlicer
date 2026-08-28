[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $PayloadDirectory,
    [string] $VersionTag = (Get-Date -Format "yyyyMMdd-HHmm"),
    [string] $OutputDirectory = "",
    [string] $MakeNsisPath = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot "output\installers" }

$payload = (Resolve-Path -LiteralPath $PayloadDirectory).Path
$requiredPayloadFiles = @(
    "OrcaSlicer\orca-slicer.exe",
    "OrcaSlicer\OrcaSlicer.dll",
    "tools\ai\orca_ai_sidecar.py",
    "tools\ai\model_input_image_quality.py",
    "03-start-orcaslicer-ai.bat",
    "setup\ai-config.bat",
    "runtime\python\python.exe"
)
foreach ($relativePath in $requiredPayloadFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $payload $relativePath) -PathType Leaf)) {
        throw "Installer payload is missing: $relativePath"
    }
}

if (-not $MakeNsisPath) {
    $makeNsisCandidates = @(
        (Join-Path $env:ProgramFiles "NSIS\makensis.exe"),
        (Join-Path ${env:ProgramFiles(x86)} "NSIS\makensis.exe"),
        (Join-Path $projectRoot "tmp\nsis-portable\Bin\makensis.exe")
    )
    $MakeNsisPath = $makeNsisCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1
}
if (-not $MakeNsisPath -or -not (Test-Path -LiteralPath $MakeNsisPath -PathType Leaf)) {
    throw "makensis.exe was not found. Pass -MakeNsisPath explicitly."
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$outputRoot = (Resolve-Path -LiteralPath $OutputDirectory).Path
$outputFile = Join-Path $outputRoot "OrcaSlicer-AI-Beta-Setup-$VersionTag.exe"
if (Test-Path -LiteralPath $outputFile) { throw "Installer already exists: $outputFile" }

$installerScript = Join-Path $projectRoot "packaging\windows-ai-test\installer.nsi"
$iconFile = Join-Path $projectRoot "resources\images\OrcaSlicer.ico"
$arguments = @(
    "/V2",
    "/DPAYLOAD_DIR=$payload",
    "/DOUTPUT_FILE=$outputFile",
    "/DVERSION_TAG=$VersionTag",
    "/DICON_FILE=$iconFile",
    $installerScript
)
if (Test-Path -LiteralPath (Join-Path $payload "setup\preconfigured-ai-credentials.marker") -PathType Leaf) {
    $arguments = $arguments[0..4] + "/DPRECONFIGURED_CREDENTIALS=1" + $arguments[5]
}
& $MakeNsisPath @arguments
if ($LASTEXITCODE -ne 0) { throw "makensis.exe failed with exit code $LASTEXITCODE" }
if (-not (Test-Path -LiteralPath $outputFile -PathType Leaf)) { throw "Installer was not created." }

$hash = (Get-FileHash -LiteralPath $outputFile -Algorithm SHA256).Hash
"$hash  $([IO.Path]::GetFileName($outputFile))" |
    Set-Content -LiteralPath "$outputFile.sha256.txt" -Encoding ASCII

[PSCustomObject]@{
    Installer = $outputFile
    SizeMB = [math]::Round((Get-Item -LiteralPath $outputFile).Length / 1MB, 2)
    SHA256 = $hash
    Payload = $payload
    MakeNsis = (Resolve-Path -LiteralPath $MakeNsisPath).Path
}
