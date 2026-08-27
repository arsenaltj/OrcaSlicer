[CmdletBinding()]
param(
    [string] $VersionTag = (Get-Date -Format "yyyyMMdd-HHmm"),
    [string] $OutputDirectory = "",
    [switch] $PreconfigureApiKeysFromEnvironment,
    [string] $PythonVersion = "3.12.10",
    [string] $PillowVersion = "12.2.0"
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
$releaseDllSource = Join-Path $projectRoot "build\src\Release\OrcaSlicer.dll"
$templateSource = Join-Path $projectRoot "packaging\windows-ai-test"
if (-not (Test-Path -LiteralPath (Join-Path $runtimeSource "orca-slicer.exe"))) { throw "Release executable is missing." }
if (-not (Test-Path -LiteralPath $releaseDllSource)) { throw "Fresh Release DLL is missing." }

New-Item -ItemType Directory -Path $packageRoot, (Join-Path $packageRoot "OrcaSlicer"), (Join-Path $packageRoot "tools\ai"), (Join-Path $packageRoot "generated_models"), (Join-Path $packageRoot "runtime\python") -Force | Out-Null

$runtimeFiles = Get-ChildItem -LiteralPath $runtimeSource -File | Where-Object {
    $_.Name -notlike "*.bak" -and $_.Name -notlike "*.phase*" -and $_.Name -notlike "*previous*"
}
$runtimeFiles | Copy-Item -Destination (Join-Path $packageRoot "OrcaSlicer")
Copy-Item -LiteralPath $releaseDllSource -Destination (Join-Path $packageRoot "OrcaSlicer\OrcaSlicer.dll") -Force
Copy-Item -LiteralPath (Join-Path $runtimeSource "resources") -Destination (Join-Path $packageRoot "OrcaSlicer\resources") -Recurse

$aiRuntimeFiles = @(
    "check_sidecar_capability.ps1",
    "openai_preprocessor.py",
    "model_provider_gateway.py",
    "model_refinement.py",
    "printable_image_pipeline.py",
    "printable_model_quality.py",
    "sampled_local_thickness.py",
    "printable_model_views.py",
    "printable_palette.py",
    "printable_visual_quality.py",
    "orca_ai_sidecar.py",
    "refresh_ai_environment.bat",
    "start_orca_ai_sidecar.bat",
    "stop_orca_ai_sidecar.ps1",
    "tripo_client.py"
)
foreach ($file in $aiRuntimeFiles) {
    Copy-Item -LiteralPath (Join-Path $projectRoot "tools\ai\$file") -Destination (Join-Path $packageRoot "tools\ai\$file")
}

Get-ChildItem -LiteralPath $templateSource -Force |
    Where-Object { $_.Name -ne "installer.nsi" } |
    Copy-Item -Destination $packageRoot -Recurse

$configTemplate = Join-Path $packageRoot "setup\ai-config.example.bat"
$configFile = Join-Path $packageRoot "setup\ai-config.bat"
if (-not (Test-Path -LiteralPath $configTemplate -PathType Leaf)) {
    throw "AI configuration template is missing."
}
Copy-Item -LiteralPath $configTemplate -Destination $configFile

if ($PreconfigureApiKeysFromEnvironment) {
    $openAIKey = [Environment]::GetEnvironmentVariable("OPENAI_API_KEY", "Process")
    $tripoKey = [Environment]::GetEnvironmentVariable("TRIPO_API_KEY", "Process")
    if (-not $openAIKey -or -not $tripoKey) {
        throw "Preconfigured packaging requires OPENAI_API_KEY and TRIPO_API_KEY in the packaging process environment."
    }
    foreach ($entry in @{ OPENAI_API_KEY = $openAIKey; TRIPO_API_KEY = $tripoKey }.GetEnumerator()) {
        if ($entry.Value -match '[\r\n%"&|<>^]') {
            throw "$($entry.Key) contains characters that cannot be safely stored in a batch configuration file."
        }
    }
    $configContent = [IO.File]::ReadAllText($configFile)
    $configContent = $configContent.Replace('set "OPENAI_API_KEY="', "set `"OPENAI_API_KEY=$openAIKey`"")
    $configContent = $configContent.Replace('set "TRIPO_API_KEY="', "set `"TRIPO_API_KEY=$tripoKey`"")
    [IO.File]::WriteAllText($configFile, $configContent, [Text.Encoding]::ASCII)
    New-Item -ItemType File -Path (Join-Path $packageRoot "setup\preconfigured-ai-credentials.marker") -Force | Out-Null
}

$runtimeCache = Join-Path $projectRoot "tmp\python-runtime-cache"
New-Item -ItemType Directory -Path $runtimeCache -Force | Out-Null
$pythonArchiveName = "python-$PythonVersion-embed-amd64.zip"
$pythonArchive = Join-Path $runtimeCache $pythonArchiveName
if (-not (Test-Path -LiteralPath $pythonArchive -PathType Leaf)) {
    Invoke-WebRequest -UseBasicParsing -Uri "https://www.python.org/ftp/python/$PythonVersion/$pythonArchiveName" -OutFile $pythonArchive
}

$pythonParts = $PythonVersion.Split('.')
$pythonTag = "cp$($pythonParts[0])$($pythonParts[1])"
$pillowWheelName = "pillow-$PillowVersion-$pythonTag-$pythonTag-win_amd64.whl"
$pillowWheel = Join-Path $runtimeCache $pillowWheelName
if (-not (Test-Path -LiteralPath $pillowWheel -PathType Leaf)) {
    $pillowMetadata = Invoke-RestMethod -Uri "https://pypi.org/pypi/pillow/$PillowVersion/json"
    $pillowArtifact = $pillowMetadata.urls | Where-Object { $_.filename -eq $pillowWheelName } | Select-Object -First 1
    if (-not $pillowArtifact) { throw "Pillow wheel was not found: $pillowWheelName" }
    Invoke-WebRequest -UseBasicParsing -Uri $pillowArtifact.url -OutFile $pillowWheel
}

Add-Type -AssemblyName System.IO.Compression.FileSystem
$bundledPythonRoot = Join-Path $packageRoot "runtime\python"
[IO.Compression.ZipFile]::ExtractToDirectory($pythonArchive, $bundledPythonRoot, $true)
[IO.Compression.ZipFile]::ExtractToDirectory($pillowWheel, $bundledPythonRoot, $true)
$pythonPathFile = Join-Path $bundledPythonRoot "python$($pythonParts[0])$($pythonParts[1])._pth"
if (-not (Test-Path -LiteralPath $pythonPathFile -PathType Leaf)) {
    throw "Bundled Python path configuration is missing: $pythonPathFile"
}
[IO.File]::AppendAllText($pythonPathFile, "..\..\tools\ai`r`n", [Text.Encoding]::ASCII)
$bundledPython = Join-Path $bundledPythonRoot "python.exe"
& $bundledPython -c "from PIL import Image; import orca_ai_sidecar; print('Bundled Python, Pillow and sidecar ready')"
if ($LASTEXITCODE -ne 0) { throw "Bundled Python could not import Pillow and the AI sidecar." }

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
    "AI sidecar protocol: orcaslicer-ai-sidecar-v5",
    "Bundled Python: $PythonVersion",
    "Bundled Pillow: $PillowVersion",
    "API credentials preconfigured: $($PreconfigureApiKeysFromEnvironment.IsPresent)"
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
