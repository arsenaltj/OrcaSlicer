param(
    [Parameter(Mandatory=$true)][string]$Destination,
    [Parameter(Mandatory=$true)][string]$MobileSamRoot,
    [Parameter(Mandatory=$true)][string]$OnnxRuntimeRoot
)
$ErrorActionPreference = 'Stop'
$boundary = Join-Path ([IO.Path]::GetFullPath($Destination)) 'boundary'
New-Item -ItemType Directory -Force -Path $boundary | Out-Null
$files = @(
    @{ Source = (Join-Path $OnnxRuntimeRoot 'capi/onnxruntime.dll'); Name = 'onnxruntime.dll'; Hash = 'c7151fd9844ad7c7d18525f1177e9ef62d91e4a6ac3583d0be700554a2b2b1d6' },
    @{ Source = (Join-Path $MobileSamRoot 'weights/mobile_sam_encoder.onnx'); Name = 'mobile_sam_encoder.onnx'; Hash = '83398d336e1e95140df654a7be83ddb35b4c78221dd984f4f62d2a78d95ace32' },
    @{ Source = (Join-Path $MobileSamRoot 'weights/mobile_sam_decoder.onnx'); Name = 'mobile_sam_decoder.onnx'; Hash = '43c7655a81b62c0f2b0e31d2bc91d3811b48d7f736ecf93b3a4a1254318241cf' }
)
foreach ($item in $files) {
    if ((Get-FileHash -LiteralPath $item.Source -Algorithm SHA256).Hash.ToLowerInvariant() -ne $item.Hash) { throw "Boundary resource hash mismatch: $($item.Name)" }
    Copy-Item -LiteralPath $item.Source -Destination (Join-Path $boundary $item.Name) -Force
}
Copy-Item -LiteralPath (Join-Path $MobileSamRoot 'LICENSE') -Destination (Join-Path $boundary 'MobileSAM-LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $OnnxRuntimeRoot 'LICENSE') -Destination (Join-Path $boundary 'ONNXRuntime-LICENSE') -Force
Copy-Item -LiteralPath (Join-Path $OnnxRuntimeRoot 'ThirdPartyNotices.txt') -Destination (Join-Path $boundary 'ONNXRuntime-ThirdPartyNotices.txt') -Force
$manifest = [ordered]@{
    schema = 'orcaslicer.native-boundary-runtime/v1'
    provider = 'mobilesam.cpu.v1'
    runtime_version = '1.22.1'
    source_revision = 'f706ad9c4eb7f219c00d9050e46328518ffb65d2'
    preprocess = 'sam-resize-half-up-rgb8-normalize-zero-pad-v2'
    purpose = 'internal-local-evaluation; commercial redistribution review incomplete'
    files = @($files | ForEach-Object { @{ name=$_.Name; sha256=$_.Hash; bytes=(Get-Item -LiteralPath (Join-Path $boundary $_.Name)).Length } })
}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $boundary 'runtime-manifest.json') -Encoding UTF8
Write-Output $boundary
