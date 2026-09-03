[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Send-EnvironmentChangedNotification {
    try {
        if (-not ('OrcaAI.EnvironmentBroadcast' -as [type])) {
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace OrcaAI {
    public static class EnvironmentBroadcast {
        [DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        public static extern IntPtr SendMessageTimeout(
            IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam,
            uint flags, uint timeout, out UIntPtr result);
    }
}
'@
        }
        $result = [UIntPtr]::Zero
        $null = [OrcaAI.EnvironmentBroadcast]::SendMessageTimeout(
            [IntPtr] 0xffff, 0x001A, [UIntPtr]::Zero, 'Environment', 0x0002, 5000, [ref] $result)
        return $true
    } catch {
        return $false
    }
}

try {
    foreach ($name in @('OPENAI_PRO_API', 'OPENAI_PRO_URL')) {
        [Environment]::SetEnvironmentVariable($name, $null, 'User')
        Remove-Item -LiteralPath "Env:$name" -ErrorAction SilentlyContinue
        if (-not [string]::IsNullOrEmpty([Environment]::GetEnvironmentVariable($name, 'User'))) {
            throw "Windows did not remove the current-user value '$name'."
        }
    }

    $broadcastSucceeded = Send-EnvironmentChangedNotification
    $machineConfigurationRemains = -not [string]::IsNullOrWhiteSpace(
        [Environment]::GetEnvironmentVariable('OPENAI_PRO_API', 'Machine')) -or
        -not [string]::IsNullOrWhiteSpace([Environment]::GetEnvironmentVariable('OPENAI_PRO_URL', 'Machine'))

    try {
        $logDirectory = Join-Path $env:LOCALAPPDATA 'OrcaSlicer\logs'
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
        $line = '{0} action=remove scope=User machine_configuration_remains={1} environment_broadcast={2}' -f `
            [DateTime]::UtcNow.ToString('o'), $machineConfigurationRemains, $broadcastSucceeded
        Add-Content -LiteralPath (Join-Path $logDirectory 'ai-config-install.log') -Value $line -Encoding utf8
    } catch {
        # Removal success must not be hidden because optional logging failed.
    }

    Write-Host 'Current-user OrcaSlicer AI PRO configuration removed.'
    if ($machineConfigurationRemains) {
        Write-Warning 'A machine-level PRO configuration still exists and may remain effective.'
    }
    Write-Host 'Completely close every OrcaSlicer window, then start OrcaSlicer again.'
    exit 0
} catch {
    Write-Error "OrcaSlicer AI PRO configuration was not removed: $($_.Exception.Message)"
    exit 1
}
