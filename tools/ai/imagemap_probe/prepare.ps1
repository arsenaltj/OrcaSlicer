param([Parameter(Mandatory=$true)][string]$Destination)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sources.json') -Raw | ConvertFrom-Json
$base = 'https://raw.githubusercontent.com/sentientstardust-dev/OrcaSlicer-ImageMap/' + $manifest.commit + '/'
foreach ($file in $manifest.files) {
    $target = Join-Path $Destination $file.path
    if ((Test-Path -LiteralPath $target) -and
        (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash.ToLowerInvariant() -eq $file.sha256) { continue }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    $temporary = $target + '.partial'
    Invoke-WebRequest -Uri ($base + $file.source_path) -OutFile $temporary
    if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.sha256) {
        throw "SHA256 mismatch: $($file.path)"
    }
    Move-Item -LiteralPath $temporary -Destination $target -Force
}
Write-Output "Verified $($manifest.files.Count) fixed upstream source/license files at $Destination"
