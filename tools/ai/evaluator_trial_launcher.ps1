$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

$root = $PSScriptRoot
$python = Join-Path $root 'python\python.exe'
$exporter = Join-Path $root 'render\evaluator_render_export.py'
if (-not (Test-Path -LiteralPath $python -PathType Leaf) -or -not (Test-Path -LiteralPath $exporter -PathType Leaf)) {
    [System.Windows.Forms.MessageBox]::Show('试用包不完整：缺少 Python 或渲染脚本。', '评测视图导出') | Out-Null
    exit 1
}

$modelDialog = New-Object System.Windows.Forms.OpenFileDialog
$modelDialog.Title = '选择要评测的生成模型 GLB'
$modelDialog.Filter = 'GLB 模型 (*.glb)|*.glb'
$modelDialog.CheckFileExists = $true
if ($modelDialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }

$folderDialog = New-Object System.Windows.Forms.FolderBrowserDialog
$folderDialog.Description = '选择评测图片的存放位置；工具会创建新的结果文件夹'
if ($folderDialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { exit 0 }

$stem = [IO.Path]::GetFileNameWithoutExtension($modelDialog.FileName)
$stem = [regex]::Replace($stem, '[^\p{L}\p{N}_.-]', '_')
$name = '{0}-Hi3DEval-{1}' -f $stem, (Get-Date -Format 'yyyyMMdd-HHmmss')
$output = Join-Path $folderDialog.SelectedPath $name
if (Test-Path -LiteralPath $output) {
    $output = Join-Path $folderDialog.SelectedPath ($name + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
}

Write-Host "源模型: $($modelDialog.FileName)"
Write-Host "输出: $output"
Write-Host '正在渲染八对视角，较大的模型可能需要几分钟...'
& $python -u $exporter $modelDialog.FileName $output
if ($LASTEXITCODE -ne 0) {
    [System.Windows.Forms.MessageBox]::Show('导出失败。请查看此窗口中的错误信息；源模型未被修改。', '评测视图导出') | Out-Null
    exit $LASTEXITCODE
}
[System.Windows.Forms.MessageBox]::Show("八对 RGB/Normal 图片已导出到：`n$output", '评测视图导出') | Out-Null
Start-Process explorer.exe -ArgumentList @($output)
