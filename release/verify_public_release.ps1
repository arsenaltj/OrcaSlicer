[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string] $ManifestPath,
    [string] $BaseUrl = 'https://3dprint.beer'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$resolvedManifest = (Resolve-Path -LiteralPath $ManifestPath).Path
$manifest = Get-Content -LiteralPath $resolvedManifest -Raw | ConvertFrom-Json
$installerPath = Join-Path (Split-Path -Parent $resolvedManifest) $manifest.installer
if (-not (Test-Path -LiteralPath $installerPath -PathType Leaf)) {
    throw "Manifest installer is missing: $installerPath"
}
$installerItem = Get-Item -LiteralPath $installerPath
$localHash = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash
if ($localHash -ne $manifest.installer_sha256) {
    throw 'Local installer hash does not match the manifest.'
}

$normalizedBase = $BaseUrl.TrimEnd('/')
$cacheToken = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
$pageResponse = Invoke-WebRequest "$normalizedBase/?release_verify=$cacheToken" -UseBasicParsing
if ($pageResponse.StatusCode -ne 200) { throw "Homepage returned $($pageResponse.StatusCode)." }
$shortSource = ([string]$manifest.source_commit).Substring(0, 10)
$createdUtc = [DateTimeOffset]::Parse(
    [string]$manifest.created_utc,
    [System.Globalization.CultureInfo]::InvariantCulture,
    [System.Globalization.DateTimeStyles]::AssumeUniversal -bor [System.Globalization.DateTimeStyles]::AdjustToUniversal
)
$builtAt = $createdUtc.ToOffset([TimeSpan]::FromHours(8)).ToString("yyyy-MM-dd HH:mm:ss 'CST'")
foreach ($requiredText in @($manifest.installer, $shortSource, $builtAt, '本次更新', '下载内部测试版')) {
    if (-not $pageResponse.Content.Contains([string]$requiredText)) {
        throw "Homepage is missing expected release text: $requiredText"
    }
}
if ($pageResponse.Content.Contains('登录后下载')) {
    throw 'Homepage unexpectedly requires login for the internal installer.'
}

Add-Type -AssemblyName System.Net.Http
$httpClient = New-Object System.Net.Http.HttpClient
$rangeRequest = $null
$rangeResponse = $null
try {
    $downloadUri = "$normalizedBase/downloads/$($manifest.installer)"
    $rangeRequest = New-Object System.Net.Http.HttpRequestMessage([System.Net.Http.HttpMethod]::Get, $downloadUri)
    $rangeRequest.Headers.Range = New-Object System.Net.Http.Headers.RangeHeaderValue(0, 15)
    $rangeResponse = $httpClient.SendAsync($rangeRequest).GetAwaiter().GetResult()
    $rangeBytes = $rangeResponse.Content.ReadAsByteArrayAsync().GetAwaiter().GetResult()
    if ([int]$rangeResponse.StatusCode -ne 206) { throw "Range download returned $([int]$rangeResponse.StatusCode)." }
    if ($rangeBytes.Length -ne 16 -or $rangeBytes[0] -ne 0x4D -or $rangeBytes[1] -ne 0x5A) {
        throw 'Range response does not contain the expected 16-byte Windows MZ prefix.'
    }
    if (-not $rangeResponse.Content.Headers.ContentRange -or
        $rangeResponse.Content.Headers.ContentRange.Length -ne $installerItem.Length) {
        throw 'Range response total size does not match the local installer.'
    }
    $checksumValues = $null
    if (-not $rangeResponse.Headers.TryGetValues('X-Checksum-SHA256', [ref]$checksumValues) -or
        @($checksumValues)[0] -ne $localHash) {
        throw 'Range response checksum header does not match the local installer.'
    }
} finally {
    if ($rangeRequest) { $rangeRequest.Dispose() }
    if ($rangeResponse) { $rangeResponse.Dispose() }
    $httpClient.Dispose()
}

$health = Invoke-RestMethod "$normalizedBase/healthz" -Method Get
if ($health.status -ne 'ok') { throw 'Public health endpoint did not return status=ok.' }

[pscustomobject]@{
    Verified      = $true
    Homepage      = "$normalizedBase/"
    Download      = "$normalizedBase/downloads/$($manifest.installer)"
    SourceCommit  = $manifest.source_commit
    BuiltAt       = $builtAt
    SizeBytes     = $installerItem.Length
    Sha256        = $localHash
    Health        = $health.status
}
