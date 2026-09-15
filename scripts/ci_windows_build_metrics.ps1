[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Start', 'Complete')]
    [string]$Action,
    [Parameter(Mandatory = $true)]
    [ValidateSet('deps-configure', 'deps-build', 'configure', 'build', 'gettext', 'install')]
    [string]$Stage,
    [long]$ExitCode = 0
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding($false)

function Write-NewJsonFile([string]$Path, $Value) {
    $bytes = $utf8.GetBytes(($Value | ConvertTo-Json -Depth 5 -Compress) + "`n")
    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::CreateNew,
        [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
}

try {
    # Only this explicit opt-in path and fixed build labels are read. Never log
    # command arguments, CMake cache contents, or the process environment.
    # IsPathRooted alone also accepts C:relative and \root-relative paths.
    $driveAbsolute = $env:ORCA_CI_METRICS_PATH -match '^[A-Za-z]:[\\/]'
    $uncAbsolute = $env:ORCA_CI_METRICS_PATH -match '^\\\\[^\\/?*:"<>|]+\\[^\\/?*:"<>|]+\\'
    if ([string]::IsNullOrWhiteSpace($env:ORCA_CI_METRICS_PATH) -or
            -not ($driveAbsolute -or $uncAbsolute)) {
        throw 'Metrics path must be absolute.'
    }
    $metricsPath = [System.IO.Path]::GetFullPath($env:ORCA_CI_METRICS_PATH)
    $statePath = $metricsPath + '.stage.json'
    $directory = [System.IO.Path]::GetDirectoryName($metricsPath)
    [System.IO.Directory]::CreateDirectory($directory) | Out-Null

    if ($Action -eq 'Start') {
        $state = [ordered]@{
            schema = 'orca.windows-build-metrics/v1'
            stage = $Stage
            start_utc = [DateTimeOffset]::UtcNow.ToString('o')
            start_timestamp = [System.Diagnostics.Stopwatch]::GetTimestamp()
            timestamp_frequency = [System.Diagnostics.Stopwatch]::Frequency
            generator = $env:ORCA_CI_METRICS_GENERATOR
            configuration = $env:ORCA_CI_METRICS_CONFIGURATION
            architecture = $env:ORCA_CI_METRICS_ARCHITECTURE
            build_directory = $env:ORCA_CI_METRICS_BUILD_DIR
            build_log_kind = $env:ORCA_CI_METRICS_BUILD_LOG_KIND
            build_log = $env:ORCA_CI_METRICS_BUILD_LOG
        }
        # An interrupted prior stage is retained; do not silently replace it.
        Write-NewJsonFile $statePath $state
        exit 0
    }

    $state = [System.IO.File]::ReadAllText($statePath, $utf8) | ConvertFrom-Json
    if ($state.schema -ne 'orca.windows-build-metrics/v1' -or $state.stage -ne $Stage -or
            $state.timestamp_frequency -ne [System.Diagnostics.Stopwatch]::Frequency) {
        throw 'Metrics stage identity changed.'
    }
    $duration = ([System.Diagnostics.Stopwatch]::GetTimestamp() - [long]$state.start_timestamp) /
        [double][System.Diagnostics.Stopwatch]::Frequency
    if ($duration -lt 0) { throw 'Metrics clock changed.' }
    $record = [ordered]@{
        schema = $state.schema
        stage = $Stage
        start_utc = $state.start_utc
        # Includes the handoff to this recorder; native detail remains in the
        # opt-in binlog or existing Ninja log referenced below.
        measurement = 'elapsed_between_phase_markers'
        duration_seconds = [Math]::Round($duration, 6)
        exit_code = $ExitCode
        generator = $state.generator
        configuration = $state.configuration
        architecture = $state.architecture
        build_directory = $state.build_directory
        includes_link = $Stage -in @('deps-build', 'build')
        build_log_kind = $state.build_log_kind
        build_log = $state.build_log
        build_log_exists = (-not [string]::IsNullOrWhiteSpace($state.build_log)) -and
            [System.IO.File]::Exists($state.build_log)
    }
    $bytes = $utf8.GetBytes(($record | ConvertTo-Json -Depth 5 -Compress) + "`n")
    $stream = [System.IO.File]::Open($metricsPath, [System.IO.FileMode]::OpenOrCreate,
        [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        if ($stream.Length -gt 1MB) { throw 'Metrics file exceeds the per-job limit.' }
        $stream.Seek(0, [System.IO.SeekOrigin]::End) | Out-Null
        $stream.Write($bytes, 0, $bytes.Length)
        $stream.Flush($true)
    } finally {
        $stream.Dispose()
    }
    [System.IO.File]::Delete($statePath)
    exit 0
} catch {
    # Do not render arbitrary exception details or environment-derived values.
    [Console]::Error.WriteLine("Could not record Windows build metrics for $Stage ($Action).")
    exit 1
}
