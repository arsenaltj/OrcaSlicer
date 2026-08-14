[CmdletBinding()]
param(
    [string] $VersionTag = (Get-Date -Format "yyyyMMdd-HHmm"),
    [string] $OutputDirectory = ""
)

$ErrorActionPreference = "Stop"
$projectRoot = Split-Path -Parent $PSScriptRoot
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $projectRoot "output\packages" }
$packageName = "OrcaSlicer-AI-Windows-x64-$VersionTag"
$packageRoot = Join-Path $OutputDirectory $packageName
$archivePath = "$packageRoot.zip"

if ((Test-Path -LiteralPath $packageRoot) -or (Test-Path -LiteralPath $archivePath)) {
    throw "Package already exists: $packageName"
}

$runtimeSource = Join-Path $projectRoot "build\OrcaSlicer"
$templateSource = Join-Path $projectRoot "packaging\windows-ai-test"
if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource "orca-slicer.exe"))) { throw "Release executable is missing." }
if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource "OrcaSlicer.dll"))) { throw "Release DLL is missing." }

New-Item -ItemType Directory -Path $packageRoot, (Join-Path $packageRoot "OrcaSlicer"), (Join-Path $packageRoot "tools\ai"), (Join-Path $packageRoot "generated_models") -Force | Out-Null

$runtimeFiles = Get-ChildItem -LiteralPath $runtimeSource -File | Where-Object {
    $_.Name -notlike "*.bak" -and $_.Name -notlike "*.phase*" -and $_.Name -notlike "*previous*"
}
$runtimeFiles | Copy-Item -Destination (Join-Path $packageRoot "OrcaSlicer")
Copy-Item -LiteralPath (Join-Path $runtimeSource "resources") -Destination (Join-Path $packageRoot "OrcaSlicer\resources") -Recurse

$aiRuntimeFiles = @(
    "check_sidecar_capability.ps1",
    "openai_preprocessor.py",
    "orca_ai_sidecar.py",
    "refresh_ai_environment.bat",
    "start_orca_ai_sidecar.bat",
    "stop_orca_ai_sidecar.ps1",
    "tripo_client.py"
)
foreach ($file in $aiRuntimeFiles) {
    Copy-Item -LiteralPath (Join-Path $projectRoot "tools\ai\$file") -Destination (Join-Path $packageRoot "tools\ai\$file")
}

Get-ChildItem -LiteralPath $templateSource -Force | Copy-Item -Destination $packageRoot -Recurse

$configTemplate = Join-Path $packageRoot "setup\ai-config.example.bat"
$configFile = Join-Path $packageRoot "setup\ai-config.bat"
if (-not (Test-Path -LiteralPath $configTemplate -PathType Leaf)) {
    throw "AI configuration template is missing."
}
Copy-Item -LiteralPath $configTemplate -Destination $configFile

# cmd.exe is unreliable with LF-only batch labels on some Windows systems.
# Normalize every shipped batch file after copying the template and runtime.
foreach ($batchFile in Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter "*.bat") {
    $content = [IO.File]::ReadAllText($batchFile.FullName)
    $content = $content -replace "`r?`n", "`r`n"
    [IO.File]::WriteAllText($batchFile.FullName, $content, [Text.Encoding]::ASCII)
}

$dllPath = Join-Path $packageRoot "OrcaSlicer\OrcaSlicer.dll"
$exePath = Join-Path $packageRoot "OrcaSlicer\orca-slicer.exe"
$commit = (& git -C $projectRoot rev-parse --short HEAD).Trim()
$dirty = if (& git -C $projectRoot status --porcelain) { "yes" } else { "no" }
$buildInfo = @(
    "Package: $packageName",
    "Created: $([DateTime]::Now.ToString('yyyy-MM-dd HH:mm:ss zzz'))",
    "Source commit: $commit",
    "Source worktree dirty: $dirty",
    "OrcaSlicer.dll SHA256: $((Get-FileHash -LiteralPath $dllPath -Algorithm SHA256).Hash)",
    "orca-slicer.exe SHA256: $((Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash)",
    "Architecture: Windows x64",
    "AI sidecar protocol: orcaslicer-ai-sidecar-v4"
)
$buildInfo | Set-Content -LiteralPath (Join-Path $packageRoot "BUILD-INFO.txt") -Encoding UTF8

$tar = Get-Command tar.exe -ErrorAction SilentlyContinue
if (-not $tar) { throw "Windows tar.exe was not found; cannot create the ZIP archive." }
& $tar.Source -a -c -f $archivePath -C $packageRoot .
if ($LASTEXITCODE -ne 0) { throw "tar.exe failed to create the ZIP archive." }
$archiveHash = (Get-FileHash -LiteralPath $archivePath -Algorithm SHA256).Hash
"$archiveHash  $([IO.Path]::GetFileName($archivePath))" | Set-Content -LiteralPath "$archivePath.sha256.txt" -Encoding ASCII

$fileMeasure = Get-ChildItem -LiteralPath $packageRoot -Recurse -File | Measure-Object Length -Sum
[PSCustomObject]@{
    PackageDirectory = $packageRoot
    Archive = $archivePath
    Files = $fileMeasure.Count
    UncompressedMB = [math]::Round($fileMeasure.Sum / 1MB, 2)
    ArchiveMB = [math]::Round((Get-Item -LiteralPath $archivePath).Length / 1MB, 2)
    SHA256 = $archiveHash
}
