[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $BuildDir,

    [string] $OutputDir,

    [string] $Revision
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$resolvedBuildDir = (Resolve-Path -LiteralPath $BuildDir).Path
$cpackConfig = Join-Path $resolvedBuildDir 'CPackConfig.cmake'
$cmakeCache = Join-Path $resolvedBuildDir 'CMakeCache.txt'

if (-not (Test-Path -LiteralPath $cpackConfig -PathType Leaf)) {
    throw "CPack configuration not found: $cpackConfig"
}
if (-not (Test-Path -LiteralPath $cmakeCache -PathType Leaf)) {
    throw "CMake cache not found: $cmakeCache"
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot 'build\windows-installer'
}
$resolvedOutputDir = [System.IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $resolvedOutputDir -Force | Out-Null

if ([string]::IsNullOrWhiteSpace($Revision)) {
    $Revision = (& git -C $repoRoot rev-parse --short=10 HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($Revision)) {
        throw 'Unable to determine the current Git revision.'
    }
}
if ($Revision -notmatch '^[0-9A-Za-z._-]+$') {
    throw "Revision contains unsupported filename characters: $Revision"
}

$cacheText = Get-Content -LiteralPath $cmakeCache -Raw
if ($cacheText -notmatch '(?m)^ORCA_AI_WINDOWS_INSTALLER:BOOL=ON\s*$') {
    throw 'The selected build directory is not configured with ORCA_AI_WINDOWS_INSTALLER=ON.'
}

$defaultsMatch = [regex]::Match($cacheText, '(?m)^ORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH=(.+)$')
if (-not $defaultsMatch.Success -or [string]::IsNullOrWhiteSpace($defaultsMatch.Groups[1].Value)) {
    throw 'The selected build directory has no ORCA_AI_INTERNAL_DEFAULTS_FILE configured.'
}
$defaultsFile = $defaultsMatch.Groups[1].Value.Trim()
if (-not (Test-Path -LiteralPath $defaultsFile -PathType Leaf)) {
    throw "Configured internal defaults payload does not exist: $defaultsFile"
}

$cpackMatch = [regex]::Match($cacheText, '(?m)^CMAKE_CPACK_COMMAND:INTERNAL=(.+)$')
if (-not $cpackMatch.Success) {
    throw 'Unable to resolve CPack from the selected CMake cache.'
}
$cpackExecutable = $cpackMatch.Groups[1].Value.Trim()
if (-not (Test-Path -LiteralPath $cpackExecutable -PathType Leaf)) {
    throw "Configured CPack executable does not exist: $cpackExecutable"
}

Push-Location $repoRoot
try {
    & cmd.exe /d /c 'scripts\run_gettext.bat'
    if ($LASTEXITCODE -ne 0) {
        throw "Localization generation failed with exit code $LASTEXITCODE."
    }
} finally {
    Pop-Location
}

$requiredCatalogs = @(
    (Join-Path $repoRoot 'resources\i18n\zh_CN\OrcaSlicer.mo'),
    (Join-Path $repoRoot 'resources\i18n\zh_TW\OrcaSlicer.mo')
)
foreach ($catalog in $requiredCatalogs) {
    if (-not (Test-Path -LiteralPath $catalog -PathType Leaf)) {
        throw "Required localization catalog was not generated: $catalog"
    }
}

$versionLine = Select-String -LiteralPath (Join-Path $repoRoot 'version.inc') -Pattern '^\s*set\(SoftFever_VERSION\s+"([^"]+)"\)' | Select-Object -First 1
if (-not $versionLine) {
    throw 'Unable to determine OrcaSlicer version from version.inc.'
}
$version = $versionLine.Matches[0].Groups[1].Value

$processorLine = Select-String -LiteralPath $cmakeCache -Pattern '^CMAKE_SYSTEM_PROCESSOR:[^=]+=(.+)$' | Select-Object -First 1
$architecture = if ($processorLine -and $processorLine.Matches[0].Groups[1].Value -eq 'ARM64') { 'arm64' } else { 'x64' }

# A short CPack name keeps the NSIS staging path below the Windows path limit.
$shortPackageName = "OrcaAI_$Revision`_$architecture"
& $cpackExecutable --config $cpackConfig -G NSIS -C Release -B $resolvedOutputDir -D "CPACK_PACKAGE_FILE_NAME=$shortPackageName"
if ($LASTEXITCODE -ne 0) {
    throw "CPack failed with exit code $LASTEXITCODE."
}

$generatedInstaller = Join-Path $resolvedOutputDir "$shortPackageName.exe"
if (-not (Test-Path -LiteralPath $generatedInstaller -PathType Leaf)) {
    throw "Expected installer was not created: $generatedInstaller"
}

$finalName = "OrcaSlicer_AI_Internal_Fast_V$version`_$Revision`_$architecture.exe"
$finalInstaller = Join-Path $resolvedOutputDir $finalName
Move-Item -LiteralPath $generatedInstaller -Destination $finalInstaller -Force

$hash = (Get-FileHash -LiteralPath $finalInstaller -Algorithm SHA256).Hash
$hashFile = "$finalInstaller.sha256"
Set-Content -LiteralPath $hashFile -Value "$hash  $finalName" -Encoding ascii

[pscustomobject]@{
    Installer = $finalInstaller
    Sha256 = $hash
    ChecksumFile = $hashFile
    Revision = $Revision
    Architecture = $architecture
    LocalizationCatalogs = $requiredCatalogs.Count
}
