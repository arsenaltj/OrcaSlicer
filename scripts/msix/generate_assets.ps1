# Generate the MSIX icons from the same company logo as the desktop application.
#
# Run once locally on Windows (re-run only if the logo changes), then commit
# the PNGs in assets/. CI never runs this script.
#
# Requires the Pillow dependency already used by the bundled application.
param(
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$source   = Join-Path $repoRoot 'resources\images\startup_logo.png'
$outDir   = Join-Path $PSScriptRoot 'assets'
New-Item -ItemType Directory -Force $outDir | Out-Null

$sizes = [ordered]@{
    'Square150x150Logo.png'                              = 150
    'Square44x44Logo.png'                                = 44
    'Square44x44Logo.targetsize-44_altform-unplated.png' = 44
    'StoreLogo.png'                                      = 50
}

$py = @'
import sys
from pathlib import Path

from PIL import Image

source, out_dir = Image.open(sys.argv[1]).convert('RGBA'), Path(sys.argv[2])
for spec in sys.argv[3:]:
    name, px = spec.rsplit('=', 1)
    px = int(px)
    source.resize((px, px), Image.Resampling.LANCZOS).save(out_dir / name)
    print(f'Wrote {name} ({px}x{px})')
'@

$renderScript = Join-Path $env:TEMP 'orca_msix_render.py'
Set-Content -Path $renderScript -Value $py -Encoding utf8
try {
    $specs = foreach ($name in $sizes.Keys) { "$name=$($sizes[$name])" }
    & $Python $renderScript $source $outDir @specs
    if ($LASTEXITCODE -ne 0) {
        throw 'MSIX company logo generation failed; use Python with Pillow installed.'
    }
}
finally {
    Remove-Item $renderScript -ErrorAction SilentlyContinue
}
