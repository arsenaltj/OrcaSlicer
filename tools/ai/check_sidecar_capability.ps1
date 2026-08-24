param(
    [Parameter(Mandatory = $true)]
    [string] $Endpoint,
    [string] $ExpectedOpenAIBaseUrl = "",
    [string] $ExpectedSidecarVersion = ""
)

$ErrorActionPreference = "Stop"

try {
    $uri = [Uri] $Endpoint
} catch {
    exit 2
}

$loopbackHosts = @("127.0.0.1", "localhost", "::1")
if ($uri.Scheme -ne "http" -or
    $loopbackHosts -notcontains $uri.Host -or
    $uri.UserInfo -ne "" -or
    $uri.Query -ne "" -or
    $uri.Fragment -ne "") {
    exit 2
}

try {
    $health = Invoke-RestMethod -Uri ($Endpoint.TrimEnd("/") + "/health") -TimeoutSec 2
} catch {
    exit 1
}

if ($health.ok -ne $true) {
    exit 1
}

if ($ExpectedOpenAIBaseUrl -ne "" -and
    ([string] $health.runtime.openai_base_url).TrimEnd("/") -ne $ExpectedOpenAIBaseUrl.TrimEnd("/")) {
    exit 3
}

if ($ExpectedSidecarVersion -ne "" -and
    ([string] $health.sidecar_version) -ne $ExpectedSidecarVersion) {
    exit 3
}

$generation = $health.capabilities.model_generation
$sources = @($generation.sources)
$formats = @($generation.artifact_formats)

if ($health.protocol_version -ne 1 -or
    $null -eq $generation -or
    $generation.available -ne $true -or
    $sources -notcontains "text" -or
    $sources -notcontains "image" -or
    $formats -notcontains "obj") {
    exit 2
}

exit 0
