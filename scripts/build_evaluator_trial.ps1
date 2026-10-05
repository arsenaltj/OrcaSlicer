<# Build a private, offline trial folder from the current local Windows runtime. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Output,
    [string]$BundledPython = '.tmp/dev/run/python',
    [string]$NumpyPython
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$target = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $target) { throw "Choose a new trial folder: $target" }
$settings = Join-Path $root '.tmp\dev\settings.json'
if (-not $NumpyPython) {
    if (-not (Test-Path -LiteralPath $settings)) { throw 'Set -NumpyPython to a Python installation with NumPy.' }
    $configured = (Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).python
    $NumpyPython = Split-Path $configured -Parent
}
$runtime = if ([IO.Path]::IsPathRooted($BundledPython)) { $BundledPython } else { Join-Path $root $BundledPython }
$numpySite = Join-Path $NumpyPython 'Lib\site-packages'
if (-not (Test-Path -LiteralPath (Join-Path $runtime 'python.exe')) -or
    -not (Test-Path -LiteralPath (Join-Path $numpySite 'numpy'))) {
    throw 'Bundled Python or NumPy source is missing.'
}
$render = Join-Path $target 'render'
$site = Join-Path $target 'python\Lib\site-packages'
New-Item -ItemType Directory -Path $render -Force | Out-Null
Copy-Item -LiteralPath $runtime -Destination (Join-Path $target 'python') -Recurse
foreach ($name in @('numpy', 'numpy.libs')) {
    Copy-Item -LiteralPath (Join-Path $numpySite $name) -Destination (Join-Path $site $name) -Recurse
}
Get-ChildItem -LiteralPath $numpySite -Directory -Filter 'numpy-*.dist-info' | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $site $_.Name) -Recurse
}
foreach ($name in @('evaluator_render_export.py', 'glb_artifact.py',
                    'local_semantic_render.py', 'local_semantic_transform.py')) {
    Copy-Item -LiteralPath (Join-Path $root "tools\ai\$name") -Destination (Join-Path $render $name)
}
$raster = Join-Path $root '.tmp\dev\run\resources\tools\ai\local_semantic_raster.dll'
if (Test-Path -LiteralPath $raster) { Copy-Item -LiteralPath $raster -Destination (Join-Path $render 'local_semantic_raster.dll') }
foreach ($name in @('evaluator_trial_launcher.cmd', 'evaluator_trial_launcher.ps1')) {
    Copy-Item -LiteralPath (Join-Path $root "tools\ai\$name") -Destination (Join-Path $target $name)
}
# Windows PowerShell 5.1 treats a BOM-less .ps1 as the system ANSI code page.
# Keep the Chinese dialog strings as UTF-8 with BOM in the deliverable.
$launcher = Join-Path $target 'evaluator_trial_launcher.ps1'
$launcherText = [IO.File]::ReadAllText($launcher, [Text.Encoding]::UTF8)
[IO.File]::WriteAllText($launcher, $launcherText, (New-Object Text.UTF8Encoding($true)))
@'
# 3D 模型评测试用导出器

双击 `evaluator_trial_launcher.cmd`，选择生成模型的 `.glb` 文件，再选择输出位置。
每次导出会创建新的文件夹，内有 `rgb_renders/view_00.png` 至 `view_07.png`、
`normal_renders/view_00.png` 至 `view_07.png` 和 `manifest.json`。

- RGB：512×512、RGB 三通道、固定浅色不透明背景、带摄影棚式主光和补光。
- Normal：512×512、RGB 三通道、相机空间编码、黑色背景。
- 八个方位角：0°、45°、…、315°；统一俯角 15°，共同中心与缩放。
- 只读取不透明、静态、三角面的 GLB；不更改源文件，不触发模型生成或联网。
- 结果目录不可覆盖。失败时检查命令窗口中的信息。

这是独立的内部测试工具，未接入 Orca 正式菜单。只有拿到此文件夹的人能使用。
'@ | Set-Content -LiteralPath (Join-Path $target '使用说明.md') -Encoding UTF8
& (Join-Path $target 'python\python.exe') -I -c "import numpy,PIL; print('trial runtime ready', numpy.__version__, PIL.__version__)"
if ($LASTEXITCODE -ne 0) { throw 'Trial Python dependency check failed.' }
Write-Host "Trial folder ready: $target"
