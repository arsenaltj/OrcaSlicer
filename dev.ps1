<#
.SYNOPSIS
Build and open a local Orca trial without a commit, handoff or package.
.EXAMPLE
./dev.ps1
.EXAMPLE
./dev.ps1 Sidecar -TestPattern test_sidecar_contract.py
.EXAMPLE
./dev.ps1 Check
.EXAMPLE
./dev.ps1 Test -TestPattern test_sidecar_mock.py
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)][ValidateSet('Run', 'Build', 'Sidecar', 'Check', 'Test', 'Review')][string]$Action = 'Run',
    [string]$BuildDir = 'build-validation',
    [ValidateRange(1, 32)][int]$Jobs = 2,
    [string[]]$TestPattern = @(),
    [switch]$Configure,
    [switch]$NoLaunch,
    [string]$BaseRef = 'origin/codex/team/integration',
    [string]$HeadRef = 'HEAD',
    [switch]$Committed
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($env:OS -ne 'Windows_NT') { throw 'dev.ps1 currently supports the Windows internal development runtime.' }
if ($Action -eq 'Test' -and -not $TestPattern.Count) { throw 'Test requires -TestPattern test_<module>.py; select the affected offline tests explicitly.' }
if ($Configure -and $Action -in @('Sidecar', 'Test')) { throw 'Use Run -Configure for native configuration changes.' }
$root = $PSScriptRoot
if ($Action -eq 'Review') {
    $reviewPython = Join-Path $root '.tmp/architecture-review/tool-env/Scripts/python.exe'
    if (-not (Test-Path -LiteralPath $reviewPython -PathType Leaf)) {
        throw 'Architecture review environment missing. See Docs/coordination/architecture-review.md for one-time setup with a full Python installation.'
    }
    $reviewArgs = @((Join-Path $root 'scripts/architecture_review.py'), '--root', $root, '--base', $BaseRef, '--head', $HeadRef)
    if (-not $Committed) { $reviewArgs += '--worktree' }
    $reviewLog = Join-Path $root '.tmp/architecture-review/latest.log'
    # Update only the deterministic README block before snapshotting the worktree.
    # Committed review must not edit a checkout or mix in uncommitted documentation.
    if (-not $Committed) {
        & $reviewPython (Join-Path $root 'scripts/architecture_review.py') --root $root --readme update *> $reviewLog
        if ($LASTEXITCODE -ne 0) { throw "README architecture update failed; see $reviewLog." }
    } else {
        Set-Content -LiteralPath $reviewLog -Value ''
    }
    & $reviewPython @reviewArgs *>> $reviewLog
    if ($LASTEXITCODE -ne 0) {
        Get-Content -LiteralPath $reviewLog -Tail 12 | Write-Host
        throw "Architecture review failed; see $reviewLog. An older report is not evidence for this attempt."
    }
    Get-Content -LiteralPath $reviewLog -Tail 1 | Write-Host
    return
}
$build = if ([IO.Path]::IsPathRooted($BuildDir)) { [IO.Path]::GetFullPath($BuildDir) } else { Join-Path $root $BuildDir }
$cachePath = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "Configured CMake cache missing: $cachePath. Select the existing tree with -BuildDir; see Docs/coordination/quick-development.md."
}
$cache = Get-Content -LiteralPath $cachePath -Raw
$pythonMatch = [regex]::Match($cache, '(?m)^Python3_EXECUTABLE:FILEPATH=(.+)$')
if (-not $pythonMatch.Success) { throw 'CMake cache is missing Python3_EXECUTABLE.' }
$python = $pythonMatch.Groups[1].Value.Trim()
$helper = Join-Path $root 'scripts/dev_runtime.py'
$checkJson = & $python -I $helper check --root $root --build $build
if ($LASTEXITCODE -ne 0) { throw 'Development preflight failed; see the specific error above.' }
$check = ($checkJson -join "`n") | ConvertFrom-Json
if ($Action -eq 'Check') {
    [pscustomobject]@{ Status = 'READY'; Source = $check.source; Build = $check.build; Runtime = $check.runtime; Data = $check.data; SourceClean = $check.source_clean; NeedsConfiguration = $check.needs_runtime_configuration }
    return
}
$local = Join-Path $root '.tmp/dev'
$runtime = $check.runtime
$data = $check.data
$ownerPath = Join-Path $local 'owner.json'
if (Test-Path -LiteralPath $ownerPath) {
    $owner = Get-Content -LiteralPath $ownerPath -Raw | ConvertFrom-Json
    if ($owner.runtime -ne $runtime -or $owner.data -ne $data) { throw 'Development runtime ownership does not match this checkout.' }
}
function Assert-RuntimeClosed {
    # Stop before touching loaded DLL/Python files. Leave save prompts and other
    # Orca instances to their owners; this script never kills processes.
    $running = @(Get-CimInstance Win32_Process | Where-Object {
        $_.ExecutablePath -and $_.ExecutablePath.Replace('\', '/').StartsWith($runtime.Replace('\', '/') + '/', [StringComparison]::OrdinalIgnoreCase)
    })
    if ($running.Count) { throw "Close this trial normally before updating its runtime. PIDs: $($running.ProcessId -join ', ')" }
}
function Assert-LaunchPortAvailable {
    $listeners = [Net.NetworkInformation.IPGlobalProperties]::GetIPGlobalProperties().GetActiveTcpListeners()
    if (@($listeners | Where-Object Port -eq 18764).Count) {
        throw 'Local startup prerequisite: port 18764 is occupied. Preserve the existing instance and close it normally before launching this trial. Build or -NoLaunch can prepare files without starting another instance.'
    }
}
New-Item -ItemType Directory -Path $local -Force | Out-Null
$lock = $null
$attempt = Join-Path $local ('logs/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $attempt | Out-Null
$result = [ordered]@{ status = 'RUNNING'; action = $Action; started_at = (Get-Date).ToString('o'); build = $check.build; runtime = $runtime; data = $data; source_identity = $check.source_identity; steps = @(); application_pid = $null; generation_submitted_by_script = $false; package_created = $false }

function Invoke-Step {
    param([string]$Name, [string]$Executable, [string[]]$Arguments)
    $stepLog = Join-Path $attempt "$Name.log"
    Write-Host "[$Name] $Executable $($Arguments -join ' ')"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    # Windows PowerShell represents native stderr as ErrorRecord even on success.
    # Native exit status, not the presence of stderr, determines the outcome.
    $previousPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @Arguments *> $stepLog
        $code = $LASTEXITCODE
    } finally { $ErrorActionPreference = $previousPreference }
    $result.steps += [ordered]@{ step = $Name; exit_code = $code; seconds = [math]::Round($timer.Elapsed.TotalSeconds, 2); log = $stepLog }
    if ($code -ne 0) {
        Get-Content -LiteralPath $stepLog -Tail 18 | Write-Host
        if ($Name -eq 'build' -and
            (Select-String -LiteralPath $stepLog -SimpleMatch 'MSB4184' -Quiet) -and
            (Select-String -LiteralPath $stepLog -SimpleMatch 'Microsoft SDKs' -Quiet)) {
            $result.failure_kind = 'windows_sdk_access'
            Write-Host 'MSBuild could not read Windows SDK metadata. Check the effective task permissions and the Microsoft SDKs directory access. If the same build also fails in a normal terminal, check the Visual Studio Windows SDK installation. Keep the incremental build directory; this MSBuild error is not an Auto-review rejection.'
        }
        throw "$Name failed (exit $code). Log: $stepLog. This script result alone is not a platform policy denial."
    }
}

try {
    $lock = [IO.File]::Open((Join-Path $local 'build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    if ($Action -in @('Run', 'Sidecar')) {
        Assert-RuntimeClosed
        if (-not $NoLaunch) { Assert-LaunchPortAvailable }
        if (-not (Test-Path -LiteralPath $ownerPath)) {
            if ($Action -eq 'Sidecar') { throw 'No development runtime yet; use dev.ps1 Run first.' }
            if ((Test-Path -LiteralPath $runtime) -and @(Get-ChildItem -LiteralPath $runtime -Force).Count) {
                throw "Existing runtime has no development ownership record: $runtime. It will not be overwritten."
            }
            @{runtime=$runtime; data=$data} | ConvertTo-Json | Set-Content -LiteralPath $ownerPath -Encoding utf8
        }
    }
    foreach ($pattern in $TestPattern) {
        Invoke-Step "test-$($result.steps.Count)" $python @('-I', (Join-Path $root 'scripts/run_ai_offline_tests.py'), '--pattern', $pattern)
    }
    if ($Action -in @('Run', 'Build')) {
        if ($Configure -or $check.needs_runtime_configuration) {
            Invoke-Step 'configure' $check.cmake @('-S', $check.source, '-B', $check.build, '-DORCA_AI_WINDOWS_INSTALLER:BOOL=ON', '-DORCA_AI_DISTRIBUTION_CHANNEL:STRING=internal', '-DORCA_AI_INTERNAL_DEFAULTS_FILE:FILEPATH=')
            $checkJson = & $python -I $helper check --root $root --build $build
            if ($LASTEXITCODE -ne 0) { throw 'Post-configuration source check failed.' }
            $check = ($checkJson -join "`n") | ConvertFrom-Json
        }
        $checkJson | Set-Content -LiteralPath (Join-Path $attempt 'before.json') -Encoding utf8
        $buildArguments = @('--build', $check.build, '--config', 'Release', '--target', 'OrcaSlicer_app_gui')
        if ($check.generator -like 'Visual Studio*') {
            $buildArguments += @('--', "/m:$Jobs", '/p:CL_MPCount=1', '/p:UseMultiToolTask=false', '/p:BuildInParallel=false', '/nologo', '/v:minimal')
        } else { $buildArguments += @('--parallel', "$Jobs") }
        Invoke-Step 'build' $check.cmake $buildArguments
        $afterJson = & $python -I $helper check --root $root --build $build
        if ($LASTEXITCODE -ne 0) { throw 'Post-build source check failed.' }
        $after = ($afterJson -join "`n") | ConvertFrom-Json
        if ($after.source_identity -ne $check.source_identity) { throw 'Source changed during build; rerun after edits finish.' }
    }
    if ($Action -eq 'Run') {
        Invoke-Step 'install' $check.cmake @('--install', $check.build, '--config', 'Release', '--prefix', $runtime)
        Invoke-Step 'runtime-check' $python @('-I', $helper, 'finish', '--root', $root, '--build', $build, '--before', (Join-Path $attempt 'before.json'))
    } elseif ($Action -eq 'Sidecar') {
        Invoke-Step 'sidecar-update' $python @('-I', $helper, 'sidecar', '--root', $root, '--build', $build)
    }
    if ($Action -in @('Run', 'Sidecar')) {
        $runtimePython = Join-Path $runtime 'python/python.exe'
        $dependencies = Get-Content -LiteralPath (Join-Path $runtime 'resources/tools/ai/orca_ai_runtime_dependencies.json') -Raw | ConvertFrom-Json
        $pillow = @($dependencies.packages | Where-Object name -eq 'Pillow')
        if ($pillow.Count -ne 1) { throw 'Runtime dependency record must contain one Pillow version.' }
        Invoke-Step 'python-check' $runtimePython @('-I', (Join-Path $runtime 'resources/tools/ai/verify_bundled_runtime.py'), '--python-root', (Join-Path $runtime 'python'), '--expect-python', $dependencies.python.version, '--expect-pillow', $pillow[0].version, '--json')
        New-Item -ItemType Directory -Path $data -Force | Out-Null
        if (-not $NoLaunch) {
            Assert-LaunchPortAvailable
            # This is the interactive trial the user needs to see and operate.
            $app = Start-Process -FilePath (Join-Path $runtime 'orca-slicer.exe') -ArgumentList @('--datadir', ('"' + $data + '"')) -WorkingDirectory $runtime -WindowStyle Normal -PassThru
            if ($app.WaitForExit(1500)) { throw "Trial application exited during startup (exit $($app.ExitCode)); inspect $data/log." }
            $result.application_pid = $app.Id
        }
    }
    $result.status = if ($Action -eq 'Test') { 'PASSED' } elseif ($result.application_pid) { 'STARTED' } else { 'PREPARED' }
} catch {
    $result.status = 'FAILED'
    $result.error = $_.Exception.Message
    throw
} finally {
    $result.finished_at = (Get-Date).ToString('o')
    $result | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $attempt 'result.json') -Encoding utf8
    if ($lock) { $lock.Dispose() }
    Write-Host "Result: $attempt/result.json"
}
[pscustomobject]@{ Status = $result.status; Action = $Action; Steps = $result.steps.Count; ApplicationPid = $result.application_pid; Result = (Join-Path $attempt 'result.json') }
