[CmdletBinding()]
param(
    [string] $PayloadPath,
    [switch] $ValidateOnly
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$allowedPayloadProperties = @('version', 'mode', 'OPENAI_PRO_API', 'OPENAI_PRO_URL')
$providerNames = @('OPENAI_PRO_API', 'OPENAI_PRO_URL')
if ([string]::IsNullOrWhiteSpace($PayloadPath)) {
    $scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
    $PayloadPath = Join-Path $scriptDirectory 'orca-ai-provisioner.json'
}

function Read-OrcaAIProvisioningPayload {
    param([Parameter(Mandatory = $true)][string] $Path)

    $resolvedPath = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
    $item = Get-Item -LiteralPath $resolvedPath
    if ($item.Length -le 0 -or $item.Length -gt 32KB) {
        throw 'The internal configuration payload has an unsupported size.'
    }

    try {
        $payload = Get-Content -LiteralPath $resolvedPath -Raw -Encoding utf8 | ConvertFrom-Json
    } catch {
        throw 'The internal configuration payload is not valid JSON.'
    }

    $propertyNames = @($payload.PSObject.Properties.Name)
    $unexpected = @($propertyNames | Where-Object { $_ -notin $allowedPayloadProperties })
    if ($unexpected.Count -gt 0) {
        throw "The internal configuration payload contains unsupported field(s): $($unexpected -join ', ')."
    }
    foreach ($required in $allowedPayloadProperties) {
        if ($required -notin $propertyNames) {
            throw "The internal configuration payload is missing '$required'."
        }
    }
    if ($payload.version -ne 1 -or $payload.mode -ne 'internal_user_environment') {
        throw 'The internal configuration payload version or mode is unsupported.'
    }

    foreach ($name in $providerNames) {
        $value = [string] $payload.$name
        if ([string]::IsNullOrWhiteSpace($value) -or $value.Length -gt 8192 -or $value.IndexOfAny([char[]] "`0`r`n") -ge 0) {
            throw "The internal configuration value '$name' is invalid."
        }
    }

    $uri = $null
    if (-not [Uri]::TryCreate([string] $payload.OPENAI_PRO_URL, [UriKind]::Absolute, [ref] $uri) -or
        $uri.Scheme -ne 'https' -or [string]::IsNullOrWhiteSpace($uri.Host) -or
        -not [string]::IsNullOrWhiteSpace($uri.UserInfo) -or
        -not [string]::IsNullOrWhiteSpace($uri.Query) -or
        -not [string]::IsNullOrWhiteSpace($uri.Fragment)) {
        throw 'OPENAI_PRO_URL must be an absolute HTTPS URL without credentials, query, or fragment.'
    }

    [pscustomobject]@{
        ApiKey = [string] $payload.OPENAI_PRO_API
        BaseUrl = ([string] $payload.OPENAI_PRO_URL).TrimEnd('/')
        Host = $uri.Host
        Port = if ($uri.IsDefaultPort) { 443 } else { $uri.Port }
    }
}

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

function Test-GatewayReachability {
    param([Parameter(Mandatory = $true)][string] $HostName, [Parameter(Mandatory = $true)][int] $Port)

    try {
        $null = [Net.Dns]::GetHostAddresses($HostName)
    } catch {
        return 'dns_failed'
    }

    $client = [Net.Sockets.TcpClient]::new()
    try {
        $connect = $client.BeginConnect($HostName, $Port, $null, $null)
        if (-not $connect.AsyncWaitHandle.WaitOne(3000)) {
            return 'tcp_timeout'
        }
        $client.EndConnect($connect)
        return 'tcp_ok'
    } catch {
        return 'tcp_failed'
    } finally {
        $client.Dispose()
    }
}

function Write-ProvisionerLog {
    param(
        [Parameter(Mandatory = $true)][string] $Action,
        [Parameter(Mandatory = $true)][string] $HostName,
        [Parameter(Mandatory = $true)][string] $NetworkStatus,
        [Parameter(Mandatory = $true)][bool] $BroadcastSucceeded
    )

    try {
        $logDirectory = Join-Path $env:LOCALAPPDATA 'OrcaSlicer\logs'
        New-Item -ItemType Directory -Path $logDirectory -Force | Out-Null
        $line = '{0} action={1} scope=User provider_host={2} network={3} environment_broadcast={4}' -f `
            [DateTime]::UtcNow.ToString('o'), $Action, $HostName, $NetworkStatus, $BroadcastSucceeded
        Add-Content -LiteralPath (Join-Path $logDirectory 'ai-config-install.log') -Value $line -Encoding utf8
    } catch {
        # Configuration success must not be rolled back because optional logging failed.
    }
}

try {
    $configuration = Read-OrcaAIProvisioningPayload -Path $PayloadPath
    if ($ValidateOnly) {
        Write-Host "Configuration package validation passed for $($configuration.Host). No settings were changed."
        exit 0
    }

    $originalKey = [Environment]::GetEnvironmentVariable('OPENAI_PRO_API', 'User')
    $originalUrl = [Environment]::GetEnvironmentVariable('OPENAI_PRO_URL', 'User')
    $settingsTouched = $false
    try {
        $settingsTouched = $true
        [Environment]::SetEnvironmentVariable('OPENAI_PRO_API', $configuration.ApiKey, 'User')
        [Environment]::SetEnvironmentVariable('OPENAI_PRO_URL', $configuration.BaseUrl, 'User')

        $storedKey = [Environment]::GetEnvironmentVariable('OPENAI_PRO_API', 'User')
        $storedUrl = [Environment]::GetEnvironmentVariable('OPENAI_PRO_URL', 'User')
        if ($storedKey -cne $configuration.ApiKey -or $storedUrl -cne $configuration.BaseUrl) {
            throw 'Windows did not retain the new current-user configuration.'
        }
    } catch {
        if ($settingsTouched) {
            [Environment]::SetEnvironmentVariable('OPENAI_PRO_API', $originalKey, 'User')
            [Environment]::SetEnvironmentVariable('OPENAI_PRO_URL', $originalUrl, 'User')
            $null = Send-EnvironmentChangedNotification
        }
        throw
    }

    $env:OPENAI_PRO_API = $configuration.ApiKey
    $env:OPENAI_PRO_URL = $configuration.BaseUrl
    $broadcastSucceeded = Send-EnvironmentChangedNotification
    $networkStatus = Test-GatewayReachability -HostName $configuration.Host -Port $configuration.Port
    Write-ProvisionerLog -Action 'install' -HostName $configuration.Host -NetworkStatus $networkStatus -BroadcastSucceeded $broadcastSucceeded

    Write-Host 'OrcaSlicer AI PRO configuration installed for the current Windows user.'
    Write-Host "Provider: $($configuration.Host)"
    Write-Host "Network check: $networkStatus (configuration remains installed even if the network is currently unavailable)"
    Write-Host 'Completely close every OrcaSlicer window, then start OrcaSlicer again.'
    exit 0
} catch {
    Write-Error "OrcaSlicer AI PRO configuration was not installed: $($_.Exception.Message)"
    exit 1
}
