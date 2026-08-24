param(
    [Parameter(Mandatory = $true)]
    [string] $Endpoint
)

$ErrorActionPreference = "Stop"

try {
    $uri = [Uri] $Endpoint
} catch {
    exit 2
}

if ($uri.Scheme -ne "http" -or
    @("127.0.0.1", "localhost", "::1") -notcontains $uri.Host -or
    $uri.Port -le 0) {
    exit 2
}

$listeners = @(Get-NetTCPConnection -State Listen -LocalPort $uri.Port -ErrorAction SilentlyContinue)
foreach ($listener in $listeners) {
    $process = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $listener.OwningProcess)
    $commandLine = [string] $process.CommandLine
    if ($commandLine -notmatch '(?:^|[\s"''\\/])tools[\\/]ai[\\/]orca_ai_sidecar\.py(?:\s|$)') {
        exit 2
    }
    $parent = Get-CimInstance Win32_Process -Filter ("ProcessId=" + $process.ParentProcessId) -ErrorAction SilentlyContinue
    if ($null -ne $parent -and
        $parent.Name -eq "cmd.exe" -and
        ([string] $parent.CommandLine) -match '(?:^|[\s"''\\/])tools[\\/]ai[\\/]start_orca_ai_sidecar\.bat(?:[\s"]|$)') {
        Stop-Process -Id $parent.ProcessId -Force -ErrorAction SilentlyContinue
    }
    Stop-Process -Id $listener.OwningProcess -Force -ErrorAction SilentlyContinue
}

$deadline = [DateTime]::UtcNow.AddSeconds(5)
do {
    $remaining = @(Get-NetTCPConnection -State Listen -LocalPort $uri.Port -ErrorAction SilentlyContinue)
    if ($remaining.Count -eq 0) {
        exit 0
    }
    Start-Sleep -Milliseconds 100
} while ([DateTime]::UtcNow -lt $deadline)

exit 1
