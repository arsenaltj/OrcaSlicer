[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ManifestPath,
    [string] $SshTarget,
    [string] $RemoteDownloadDir = '/srv/3dprint-beer/data/downloads',
    [string] $RemoteOwner = 'web',
    [string] $RemoteGroup = 'web',
    [switch] $AllowSourceMismatch,
    [switch] $ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
foreach ($field in @('installer', 'installer_sha256', 'source_commit', 'package_revision', 'distribution_channel')) {
    if (-not $manifest.PSObject.Properties[$field] -or [string]::IsNullOrWhiteSpace([string]$manifest.$field)) {
        throw "Release manifest is missing field: $field"
    }
}
if ($manifest.distribution_channel -ne 'internal' -or $manifest.source_commit -notmatch '^[0-9a-f]{40}$' -or
    $manifest.installer_sha256 -notmatch '^[0-9A-Fa-f]{64}$') {
    throw 'Release manifest identity is invalid or is not an internal release.'
}
if ($manifest.installer -notmatch '^[0-9A-Za-z._-]+\.exe$') {
    throw "Unsafe installer filename in manifest: $($manifest.installer)"
}

$installerPath = Join-Path (Split-Path -Parent $resolvedManifest) $manifest.installer
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Manifest installer does not exist beside the manifest: $installerPath"
}
$installerItem = Get-Item -LiteralPath $installerPath
$localHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
if ($localHash -ne $manifest.installer_sha256) {
    throw 'Installer SHA-256 does not match the release manifest.'
}

$gitHead = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $gitHead -notmatch '^[0-9a-f]{40}$') {
    throw 'Unable to read the current repository source identity.'
}
$gitStatus = @(& git -C $repoRoot status --porcelain --untracked-files=all)
if ($LASTEXITCODE -ne 0) { throw 'Unable to verify the current worktree.' }
if (-not $AllowSourceMismatch -and ($gitHead -ne $manifest.source_commit -or $gitStatus.Count -gt 0)) {
    throw 'Current clean HEAD must equal manifest source_commit before upload. Rebuild or use -AllowSourceMismatch only for read-only validation of an older artifact.'
}

$validationResult = [pscustomobject]@{
    Ready          = $true
    Manifest       = $resolvedManifest
    Installer      = $installerItem.FullName
    SizeBytes      = $installerItem.Length
    Sha256         = $localHash
    SourceCommit   = $manifest.source_commit
    CurrentHead    = $gitHead
    SourceMatches  = ($gitHead -eq $manifest.source_commit)
}
if ($ValidateOnly) {
    $validationResult
    return
}

if ($AllowSourceMismatch) {
    throw '-AllowSourceMismatch is permitted only together with -ValidateOnly.'
}

if ([string]::IsNullOrWhiteSpace($SshTarget) -or $SshTarget -notmatch '^[0-9A-Za-z_.@:-]+$') {
    throw 'Pass a shell-safe local SSH alias or user@host through -SshTarget.'
}
if ($RemoteDownloadDir -notmatch '^/[0-9A-Za-z._/-]+$' -or
    $RemoteOwner -notmatch '^[0-9A-Za-z._-]+$' -or $RemoteGroup -notmatch '^[0-9A-Za-z._-]+$') {
    throw 'Remote directory, owner, or group contains unsupported shell characters.'
}

$remoteTemp = "/tmp/$($manifest.installer).upload-$([Guid]::NewGuid().ToString('N'))"
$remoteFinal = "$($RemoteDownloadDir.TrimEnd('/'))/$($manifest.installer)"
& scp $installerPath "${SshTarget}:$remoteTemp"
if ($LASTEXITCODE -ne 0) {
    throw "SCP upload failed with exit code $LASTEXITCODE."
}

$remoteCommand = 'set -eu; size=$(stat -c %s -- "{0}"); hash=$(sha256sum -- "{0}" | cut -d " " -f 1); test "$size" = "{1}"; test "$hash" = "{2}"; install -o "{3}" -g "{4}" -m 0644 -- "{0}" "{5}"; rm -f -- "{0}"; stat -c "%n %s bytes %U:%G %a" -- "{5}"; sha256sum -- "{5}"' -f `
    $remoteTemp, $installerItem.Length, $localHash.ToLowerInvariant(), $RemoteOwner, $RemoteGroup, $remoteFinal
& ssh $SshTarget $remoteCommand
if ($LASTEXITCODE -ne 0) {
    throw "Remote verification or installation failed with exit code $LASTEXITCODE. The unique temporary upload was left for diagnosis: $remoteTemp"
}

[pscustomobject]@{
    Uploaded       = $true
    SshTarget      = $SshTarget
    RemoteInstaller = $remoteFinal
    SizeBytes      = $installerItem.Length
    Sha256         = $localHash
    SourceCommit   = $manifest.source_commit
}
