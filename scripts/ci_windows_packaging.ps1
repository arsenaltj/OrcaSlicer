#Requires -Version 7.2
<#
Package an existing hosted Windows build without modifying its install tree.
The baseline profile reproduces the former serial work, including its unused
portable ZIP. The optimized profile omits that ZIP, uses -mx3 for PDBs and runs
at most two packaging processes. All output and MSIX staging is isolated per
invocation; files are published only after every package succeeds.

Use distinct -OutputDirectory and -MetricsPath values when comparing profiles
against the same build. No files are created by -DryRun. -CancellationFile is
an optional absolute sentinel path that an external controller may create.
Normal cancellation/timeout/failure stops this invocation's live child trees.
A forcibly terminated host or a tool that exits after detaching descendants
still depends on runner process cleanup. The invoked packaging tools normally
wait synchronously for their children.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._+-]*$')][string]$Version,
    [ValidateSet('x64', 'arm64')][string]$Architecture = 'x64',
    [string]$OutputDirectory = '',
    [Parameter(Mandatory)][string]$MetricsPath,
    [ValidateSet('baseline', 'optimized')][string]$Profile = 'optimized',
    [string]$IdentityName = 'OrcaSlicer.OrcaSlicer',
    [string]$Publisher = 'CN=38F7EA55-C73B-4072-B3B2-C8E0EA15BB82',
    [string]$PublisherDisplayName = 'OrcaSlicer',
    [ValidateRange(1, 14400)][int]$TimeoutSeconds = 3600,
    [string]$CancellationFile = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Full-Path([string]$Value, [string]$Label) {
    if (-not [IO.Path]::IsPathFullyQualified($Value)) { throw "$Label must be an absolute path" }
    return [IO.Path]::GetFullPath($Value)
}

function Find-Tool([string]$Name, [string]$Fallback = '') {
    $command = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    if ($Fallback -and (Test-Path -LiteralPath $Fallback -PathType Leaf)) { return $Fallback }
    throw "Required packaging tool not found: $Name"
}

function File-Manifest([string]$Root, [string]$Filter = '*', [switch]$Recurse) {
    return @(Get-ChildItem -LiteralPath $Root -File -Filter $Filter -Recurse:$Recurse |
        Sort-Object FullName | ForEach-Object {
            [ordered]@{
                path = [IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/')
                size_bytes = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            }
        })
}

$BuildDir = Full-Path $BuildDir 'BuildDir'
$MetricsPath = Full-Path $MetricsPath 'MetricsPath'
$explicitOutput = -not [string]::IsNullOrEmpty($OutputDirectory)
if (-not $explicitOutput) { $OutputDirectory = $BuildDir }
$OutputDirectory = Full-Path $OutputDirectory 'OutputDirectory'
if ($CancellationFile) { $CancellationFile = Full-Path $CancellationFile 'CancellationFile' }
if (-not $IsWindows) { throw 'Windows packaging requires a Windows host' }
if (-not (Test-Path -LiteralPath $BuildDir -PathType Container)) { throw 'BuildDir does not exist' }
if (Test-Path -LiteralPath $MetricsPath) { throw 'MetricsPath already exists; preserve prior measurements' }
if ($CancellationFile -and $CancellationFile -eq $MetricsPath) { throw 'CancellationFile must differ from MetricsPath' }

$installDir = Join-Path $BuildDir 'OrcaSlicer'
$pdbDir = Join-Path $BuildDir 'src/Release'
$cpackConfig = Join-Path $BuildDir 'CPackConfig.cmake'
foreach ($required in @($cpackConfig, (Join-Path $installDir 'orca-slicer.exe'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing packaging input: $required" }
}
if ($Architecture -eq 'x64' -and -not @(Get-ChildItem -LiteralPath $pdbDir -Filter '*.pdb' -File -ErrorAction SilentlyContinue).Count) {
    throw 'The x64 build contains no PDB files'
}
$cpack = Find-Tool 'cpack.exe'
$sevenZip = Find-Tool '7z.exe' (Join-Path $env:ProgramFiles '7-Zip/7z.exe')
$pwsh = (Get-Process -Id $PID).Path
$msixScript = Join-Path $PSScriptRoot 'msix/build_msix.ps1'
if (-not (Test-Path -LiteralPath $msixScript -PathType Leaf)) { throw 'MSIX packaging script missing' }
$invocation = Join-Path $BuildDir ('.ci-windows-packaging/' + [guid]::NewGuid().ToString('N'))
$pdbName = "Debug_PDB_${Version}_for_developers_only.7z"
$portableName = "OrcaSlicer_Windows_${Version}_${Architecture}_portable.zip"
$msixName = "OrcaSlicer_Windows_MSIX_${Version}_${Architecture}.msix"
$pdbDestination = if ($explicitOutput) { Join-Path $OutputDirectory $pdbName } else { Join-Path $pdbDir $pdbName }
$jobs = [Collections.Generic.List[object]]::new()

function Add-PackageJob([string]$Name, [string]$Executable, [string[]]$Arguments, [string]$WorkingDirectory) {
    $jobs.Add(@{
        data = [ordered]@{
            name = $Name; executable = $Executable; arguments = $Arguments
            working_directory = $WorkingDirectory; status = 'pending'
            stdout = (Join-Path $invocation "$Name.stdout.log")
            stderr = (Join-Path $invocation "$Name.stderr.log")
            started_at = $null; elapsed_seconds = $null; exit_code = $null
        }
        process = $null; started = $false; stdoutTask = $null; stderrTask = $null; watch = $null
    })
}

$nsisDirectory = Join-Path $invocation 'nsis'
Add-PackageJob 'nsis' $cpack @('--config', $cpackConfig, '-G', 'NSIS', '-B', $nsisDirectory) $BuildDir
if ($Profile -eq 'baseline') {
    Add-PackageJob 'portable-zip' $sevenZip @('a', '-tzip', (Join-Path $invocation $portableName), $installDir) $BuildDir
}
if ($Architecture -eq 'x64') {
    $compression = if ($Profile -eq 'baseline') { '-mx9' } else { '-mx3' }
    Add-PackageJob 'pdb' $sevenZip @('a', '-m0=lzma2', $compression, (Join-Path $invocation $pdbName), '*.pdb') $pdbDir
}
Add-PackageJob 'msix' $pwsh @(
    '-NoProfile', '-NonInteractive', '-File', $msixScript,
    '-InstallDir', $installDir, '-OutputPath', (Join-Path $invocation $msixName),
    '-Architecture', $Architecture, '-StagingDir', (Join-Path $invocation 'msix-staging'),
    '-IdentityName', $IdentityName, '-Publisher', $Publisher, '-PublisherDisplayName', $PublisherDisplayName
) (Split-Path $PSScriptRoot -Parent)

$metrics = [ordered]@{
    schema_version = 1; profile = $Profile; architecture = $Architecture; version = $Version
    build_directory = $BuildDir; output_directory = $OutputDirectory; invocation_directory = $invocation
    max_parallel = $(if ($Profile -eq 'baseline') { 1 } else { 2 })
    status = 'planned'; started_at = $null; elapsed_seconds = $null; error = $null
    input_manifest = $null; inputs_unchanged = $null; jobs = @($jobs | ForEach-Object { $_.data })
    artifacts = @(); msix_staging_manifest = @(); cleanup_errors = @()
}
if ($DryRun) { $metrics | ConvertTo-Json -Depth 12; return }

function Assert-NotCancelled {
    if ($CancellationFile -and (Test-Path -LiteralPath $CancellationFile)) {
        $metrics.status = 'cancelled'
        throw 'Packaging cancelled by sentinel'
    }
    if ($totalWatch.Elapsed.TotalSeconds -gt $TimeoutSeconds) {
        $metrics.status = 'timed_out'
        throw "Packaging exceeded timeout of $TimeoutSeconds seconds"
    }
}

function Collect-Job($Job, [switch]$Cleanup) {
    $Job.data.elapsed_seconds = [math]::Round($Job.watch.Elapsed.TotalSeconds, 3)
    $Job.data.exit_code = $Job.process.ExitCode
    foreach ($task in @($Job.stdoutTask, $Job.stderrTask)) {
        if ($Cleanup) {
            if (-not $task.Wait(10000)) { throw "Output stream did not close for $($Job.data.name)" }
        }
        else {
            while (-not $task.Wait(100)) { Assert-NotCancelled }
        }
    }
    [IO.File]::WriteAllText($Job.data.stdout, $Job.stdoutTask.GetAwaiter().GetResult())
    [IO.File]::WriteAllText($Job.data.stderr, $Job.stderrTask.GetAwaiter().GetResult())
    $Job.data.status = if ($Job.process.ExitCode -eq 0) { 'succeeded' } else { 'failed' }
}

$totalWatch = [Diagnostics.Stopwatch]::StartNew()
$metrics.started_at = [DateTime]::UtcNow.ToString('o')
$metrics.status = 'running'
$failure = $null
New-Item -ItemType Directory -Path $invocation | Out-Null
try {
    Assert-NotCancelled
    $metrics.input_manifest = [ordered]@{
        install = @(File-Manifest $installDir -Recurse)
        pdb = $(if ($Architecture -eq 'x64') { @(File-Manifest $pdbDir '*.pdb') } else { @() })
        cpack_config_sha256 = (Get-FileHash -LiteralPath $cpackConfig -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    New-Item -ItemType Directory -Path $nsisDirectory | Out-Null
    while (@($jobs | Where-Object { $_.data.status -in @('pending', 'running') }).Count) {
        Assert-NotCancelled
        foreach ($job in @($jobs | Where-Object { $_.data.status -eq 'running' })) {
            if ($job.process.HasExited) {
                Collect-Job $job
                if ($job.data.exit_code -ne 0) { throw "Packaging job $($job.data.name) failed with exit code $($job.data.exit_code); see $($job.data.stderr)" }
            }
        }
        $running = @($jobs | Where-Object { $_.data.status -eq 'running' }).Count
        foreach ($job in @($jobs | Where-Object { $_.data.status -eq 'pending' })) {
            if ($running -ge $metrics.max_parallel) { break }
            Assert-NotCancelled
            $info = [Diagnostics.ProcessStartInfo]::new()
            $info.FileName = $job.data.executable
            $info.WorkingDirectory = $job.data.working_directory
            $info.UseShellExecute = $false
            $info.CreateNoWindow = $true
            $info.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
            $info.RedirectStandardOutput = $true
            $info.RedirectStandardError = $true
            foreach ($argument in $job.data.arguments) { $info.ArgumentList.Add($argument) }
            $job.process = [Diagnostics.Process]::new()
            $job.process.StartInfo = $info
            $job.watch = [Diagnostics.Stopwatch]::StartNew()
            $job.data.started_at = [DateTime]::UtcNow.ToString('o')
            try {
                if (-not $job.process.Start()) { throw "Could not start packaging job $($job.data.name)" }
                $job.started = $true
            }
            catch { $job.data.status = 'start_failed'; throw }
            $job.data.status = 'running'
            $job.stdoutTask = $job.process.StandardOutput.ReadToEndAsync()
            $job.stderrTask = $job.process.StandardError.ReadToEndAsync()
            $running++
        }
        if (@($jobs | Where-Object { $_.data.status -eq 'running' }).Count) { Start-Sleep -Milliseconds 100 }
    }
    Assert-NotCancelled
    $after = [ordered]@{
        install = @(File-Manifest $installDir -Recurse)
        pdb = $(if ($Architecture -eq 'x64') { @(File-Manifest $pdbDir '*.pdb') } else { @() })
        cpack_config_sha256 = (Get-FileHash -LiteralPath $cpackConfig -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    $metrics.inputs_unchanged = ($metrics.input_manifest | ConvertTo-Json -Depth 8 -Compress) -ceq ($after | ConvertTo-Json -Depth 8 -Compress)
    if (-not $metrics.inputs_unchanged) { throw 'Packaging inputs changed during the measurement' }
    $installers = @(Get-ChildItem -LiteralPath $nsisDirectory -Filter 'OrcaSlicer*.exe' -File)
    if ($installers.Count -ne 1) { throw "Expected exactly one fresh NSIS installer; found $($installers.Count)" }
    $publications = [Collections.Generic.List[object]]::new()
    $publications.Add(@{ kind = 'nsis'; source = $installers[0].FullName; destination = (Join-Path $OutputDirectory $installers[0].Name) })
    $publications.Add(@{ kind = 'msix'; source = (Join-Path $invocation $msixName); destination = (Join-Path $OutputDirectory $msixName) })
    if ($Architecture -eq 'x64') { $publications.Add(@{ kind = 'pdb'; source = (Join-Path $invocation $pdbName); destination = $pdbDestination }) }
    if ($Profile -eq 'baseline') { $publications.Add(@{ kind = 'unused-portable-zip'; source = (Join-Path $invocation $portableName); destination = (Join-Path $OutputDirectory $portableName) }) }
    foreach ($publication in $publications) {
        if (-not (Test-Path -LiteralPath $publication.source -PathType Leaf) -or (Get-Item -LiteralPath $publication.source).Length -eq 0) {
            throw "Missing or empty output for $($publication.kind)"
        }
        if (Test-Path -LiteralPath $publication.destination) { throw "Output already exists: $($publication.destination)" }
        if ($publication.destination -eq $MetricsPath) { throw 'MetricsPath collides with a package destination' }
    }
    $metrics.msix_staging_manifest = @(File-Manifest (Join-Path $invocation 'msix-staging') -Recurse)
    foreach ($publication in $publications) {
        Assert-NotCancelled
        [IO.Directory]::CreateDirectory((Split-Path $publication.destination -Parent)) | Out-Null
        # File.Copy(overwrite: false) also rejects a concurrently created output.
        [IO.File]::Copy($publication.source, $publication.destination, $false)
        $metrics.artifacts += [ordered]@{
            kind = $publication.kind; path = $publication.destination
            size_bytes = (Get-Item -LiteralPath $publication.destination).Length
            sha256 = (Get-FileHash -LiteralPath $publication.destination -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    }
    $metrics.status = 'succeeded'
}
catch {
    $failure = $_
    if ($metrics.status -eq 'running') { $metrics.status = 'failed' }
    $metrics.error = $_.Exception.Message
}
finally {
    foreach ($job in $jobs) {
        if ($null -ne $job.process) {
            try {
                if ($job.started) {
                    if (-not $job.process.HasExited) {
                        $job.process.Kill($true)
                        if (-not $job.process.WaitForExit(10000)) { throw "Could not stop packaging job $($job.data.name)" }
                    }
                    if ($job.data.status -eq 'running') {
                        Collect-Job $job -Cleanup
                        $job.data.status = 'terminated'
                    }
                }
            }
            catch {
                $metrics.cleanup_errors += "$($job.data.name): $($_.Exception.Message)"
                if (-not $failure) { $failure = $_ }
                if ($metrics.status -eq 'succeeded') { $metrics.status = 'failed' }
            }
            finally { $job.process.Dispose() }
        }
    }
    $metrics.elapsed_seconds = [math]::Round($totalWatch.Elapsed.TotalSeconds, 3)
    [IO.Directory]::CreateDirectory((Split-Path $MetricsPath -Parent)) | Out-Null
    $json = $metrics | ConvertTo-Json -Depth 12
    $stream = [IO.File]::Open($MetricsPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $bytes = [Text.UTF8Encoding]::new($false).GetBytes($json + "`n")
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    }
    finally { $stream.Dispose() }
}
if ($failure) { throw $failure }
Write-Output "Packaging succeeded ($Profile): $MetricsPath"
