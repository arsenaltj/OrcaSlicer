[CmdletBinding()]
param(
    [string] $OutputDir,
    [ValidateSet('Effective', 'Process')]
    [string] $KeySourceScope = 'Effective',
    [string] $BaseUrl
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$templateDirectory = Join-Path $PSScriptRoot 'internal-ai-provisioner'
$verifiedBaseUrl = 'https://v.3dprint.beer/managed-ai/v1'

function Get-ConfiguredValue {
    param([Parameter(Mandatory = $true)][string] $Name, [Parameter(Mandatory = $true)][string] $Scope)

    $value = [Environment]::GetEnvironmentVariable($Name, 'Process')
    if (-not [string]::IsNullOrWhiteSpace($value) -or $Scope -eq 'Process') {
        return $value
    }
    $value = [Environment]::GetEnvironmentVariable($Name, 'User')
    if (-not [string]::IsNullOrWhiteSpace($value)) {
        return $value
    }
    return [Environment]::GetEnvironmentVariable($Name, 'Machine')
}

function Assert-SupportedSetting {
    param([Parameter(Mandatory = $true)][string] $Name, [AllowEmptyString()][string] $Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        throw "$Name is not configured in the selected environment scope."
    }
    if ($Value.Length -gt 8192 -or $Value.IndexOfAny([char[]] "`0`r`n") -ge 0) {
        throw "$Name contains an unsupported value."
    }
}

function Get-Sha256Hex {
    param([Parameter(Mandatory = $true)][string] $Path)

    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '')
    } finally {
        $algorithm.Dispose()
        $stream.Dispose()
    }
}

$apiKey = Get-ConfiguredValue -Name 'OPENAI_PRO_API' -Scope $KeySourceScope
Assert-SupportedSetting -Name 'OPENAI_PRO_API' -Value $apiKey

if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    $BaseUrl = Get-ConfiguredValue -Name 'OPENAI_PRO_URL' -Scope $KeySourceScope
}
if ([string]::IsNullOrWhiteSpace($BaseUrl)) {
    $BaseUrl = $verifiedBaseUrl
}
Assert-SupportedSetting -Name 'OPENAI_PRO_URL' -Value $BaseUrl
$BaseUrl = $BaseUrl.TrimEnd('/')

$baseUri = $null
if (-not [Uri]::TryCreate($BaseUrl, [UriKind]::Absolute, [ref] $baseUri) -or
    $baseUri.Scheme -ne 'https' -or [string]::IsNullOrWhiteSpace($baseUri.Host) -or
    -not [string]::IsNullOrWhiteSpace($baseUri.UserInfo) -or
    -not [string]::IsNullOrWhiteSpace($baseUri.Query) -or
    -not [string]::IsNullOrWhiteSpace($baseUri.Fragment)) {
    throw 'OPENAI_PRO_URL must be an absolute HTTPS URL without credentials, query, or fragment.'
}

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $repoRoot 'build\internal-ai-provisioner'
}
$resolvedOutputDir = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $resolvedOutputDir -Force | Out-Null

$sourceCommit = (& git -C $repoRoot rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sourceCommit -notmatch '^[0-9a-fA-F]{40}$') {
    throw 'Unable to resolve the OrcaSlicer source commit.'
}
$shortCommit = $sourceCommit.Substring(0, 10).ToLowerInvariant()
$timestamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$bundleBaseName = "OrcaSlicer_AI_PRO_Config_${shortCommit}_${timestamp}"
$stagingRoot = Join-Path $resolvedOutputDir ".$bundleBaseName.$([Guid]::NewGuid().ToString('N'))"
$bundlePath = Join-Path $resolvedOutputDir "$bundleBaseName.zip"
if (Test-Path -LiteralPath $bundlePath) {
    throw "Output bundle already exists: $bundlePath"
}

New-Item -ItemType Directory -Path $stagingRoot | Out-Null
try {
    foreach ($name in @(
        'Install-OrcaAIConfig.ps1',
        'Remove-OrcaAIConfig.ps1',
        'Install-OrcaAIConfig.cmd',
        'Remove-OrcaAIConfig.cmd'
    )) {
        Copy-Item -LiteralPath (Join-Path $templateDirectory $name) -Destination (Join-Path $stagingRoot $name)
    }

    $payload = [ordered]@{
        version = 1
        mode = 'internal_user_environment'
        OPENAI_PRO_API = $apiKey
        OPENAI_PRO_URL = $BaseUrl
    }
    $payload | ConvertTo-Json -Depth 3 | Set-Content `
        -LiteralPath (Join-Path $stagingRoot 'orca-ai-provisioner.json') -Encoding utf8

    $manifest = [ordered]@{
        schema_version = 1
        created_utc = [DateTime]::UtcNow.ToString('o')
        source_commit = $sourceCommit.ToLowerInvariant()
        target_scope = 'User'
        provider_host = $baseUri.Host
        provider_base_url = $BaseUrl
        configured_variables = @('OPENAI_PRO_API', 'OPENAI_PRO_URL')
        contains_extractable_credential = $true
        paid_request_performed = $false
    }
    $manifest | ConvertTo-Json -Depth 4 | Set-Content `
        -LiteralPath (Join-Path $stagingRoot 'manifest.json') -Encoding utf8

$readme = @"
OrcaSlicer AI PRO internal configuration bundle

Install: double-click Install-OrcaAIConfig.cmd.
Remove: double-click Remove-OrcaAIConfig.cmd. Removal affects only the two
current-user PRO environment variables installed by this bundle.

After installation, completely close all OrcaSlicer windows and start it again.
Diagnostic log: %LOCALAPPDATA%\OrcaSlicer\logs\ai-config-install.log

Warning: this bundle contains an extractable internal API credential. Distribute
it only through an authorized internal channel. Do not upload it to GitHub, a
public download site, or an external chat. Installation performs only DNS/TCP
network checks and does not call a paid generation API.
"@
    $readme | Set-Content -LiteralPath (Join-Path $stagingRoot 'README.txt') -Encoding utf8

    Compress-Archive -Path (Join-Path $stagingRoot '*') -DestinationPath $bundlePath -CompressionLevel Optimal
} finally {
    $resolvedStagingRoot = [IO.Path]::GetFullPath($stagingRoot)
    if ($resolvedStagingRoot.StartsWith($resolvedOutputDir.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase) -and
        (Test-Path -LiteralPath $resolvedStagingRoot)) {
        Remove-Item -LiteralPath $resolvedStagingRoot -Recurse -Force
    }
}

$bundleHash = Get-Sha256Hex -Path $bundlePath
$checksumPath = "$bundlePath.sha256"
Set-Content -LiteralPath $checksumPath -Value "$bundleHash  $([IO.Path]::GetFileName($bundlePath))" -Encoding ascii

[pscustomobject]@{
    Bundle = $bundlePath
    Sha256 = $bundleHash
    ChecksumFile = $checksumPath
    SourceCommit = $sourceCommit.ToLowerInvariant()
    ProviderHost = $baseUri.Host
    TargetScope = 'User'
    ContainsExtractableCredential = $true
}
