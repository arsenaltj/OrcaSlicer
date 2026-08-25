[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
$packageRoot = Split-Path -Parent $PSScriptRoot
$failures = [System.Collections.Generic.List[string]]::new()

function Get-Setting {
    param([Parameter(Mandatory = $true)][string] $Name)
    $value = [Environment]::GetEnvironmentVariable($Name, "Process")
    return [string] $value
}

function Check-File {
    param([Parameter(Mandatory = $true)][string] $RelativePath)
    $path = Join-Path $packageRoot $RelativePath
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        Write-Host "[OK] $RelativePath"
    } else {
        Write-Host "[MISSING] $RelativePath" -ForegroundColor Red
        $failures.Add("Missing $RelativePath")
    }
}

Write-Host "OrcaSlicer AI package environment check" -ForegroundColor Cyan
Check-File "OrcaSlicer\orca-slicer.exe"
Check-File "OrcaSlicer\OrcaSlicer.dll"
Check-File "tools\ai\orca_ai_sidecar.py"
Check-File "tools\ai\openai_preprocessor.py"
Check-File "tools\ai\model_provider_gateway.py"
Check-File "tools\ai\model_refinement.py"
Check-File "tools\ai\printable_image_pipeline.py"
Check-File "tools\ai\printable_model_quality.py"
Check-File "tools\ai\sampled_local_thickness.py"
Check-File "tools\ai\printable_model_views.py"
Check-File "tools\ai\printable_palette.py"
Check-File "tools\ai\printable_visual_quality.py"
Check-File "tools\ai\tripo_client.py"

$python = Get-Setting "PYTHON_EXE"
if (-not $python) {
    $command = Get-Command python.exe -ErrorAction SilentlyContinue
    if ($command) { $python = $command.Source }
}
if (-not $python -or -not (Test-Path -LiteralPath $python -PathType Leaf)) {
    Write-Host "[MISSING] Python 3.10+" -ForegroundColor Red
    $failures.Add("Python 3.10+ was not found")
} else {
    $version = & $python -c "import sys; print('.'.join(map(str, sys.version_info[:3])))"
    if ($LASTEXITCODE -ne 0) {
        $failures.Add("Python could not run")
    } else {
        Write-Host "[OK] Python $version"
        & $python -c "from PIL import Image; print(Image.__version__)" *> $null
        if ($LASTEXITCODE -eq 0) {
            Write-Host "[OK] Pillow"
        } else {
            Write-Host "[MISSING] Pillow" -ForegroundColor Red
            $failures.Add("Pillow is not installed")
        }
    }
}

$openAIBase = Get-Setting "OPENAI_BASE_URL"
$tripoBase = Get-Setting "TRIPO_API_BASE"
$openAIKey = Get-Setting "OPENAI_API_KEY"
$tripoKey = Get-Setting "TRIPO_API_KEY"

if (-not $openAIBase) { $openAIBase = "https://laotie.dev" }
if (-not $tripoBase) { $tripoBase = "https://openapi.tripo3d.com/v3" }

if ($openAIBase -match '^https://[^\s]+$') { Write-Host "[OK] OPENAI_BASE_URL=$openAIBase" } else { $failures.Add("OPENAI_BASE_URL is missing or is not HTTPS") }
if ($tripoBase -match '^https://[^\s]+$') { Write-Host "[OK] TRIPO_API_BASE=$tripoBase" } else { $failures.Add("TRIPO_API_BASE is missing or is not HTTPS") }
if ($openAIKey) { Write-Host "[OK] OPENAI_API_KEY is configured (hidden)" } else { $failures.Add("OPENAI_API_KEY is missing in setup\ai-config.bat") }
if ($tripoKey) { Write-Host "[OK] TRIPO_API_KEY is configured (hidden)" } else { $failures.Add("TRIPO_API_KEY is missing in setup\ai-config.bat") }

$modelDefaults = @{
    OPENAI_TEXT_MODEL = "gpt-5.4"
    OPENAI_IMAGE_MODEL = "gpt-image-2"
    TRIPO_MODEL = "v3.1-20260211"
}
foreach ($name in "OPENAI_TEXT_MODEL", "OPENAI_IMAGE_MODEL", "TRIPO_MODEL") {
    $value = Get-Setting $name
    if (-not $value) { $value = $modelDefaults[$name] }
    Write-Host "[OK] $name=$value"
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "Environment check failed:" -ForegroundColor Red
    $failures | ForEach-Object { Write-Host "- $_" }
    exit 1
}

Write-Host ""
Write-Host "Local dependencies and settings passed. The next check starts only the local sidecar and does not create a paid task." -ForegroundColor Green
exit 0
