#Requires -Version 7.2
<# Capture a small, explicit environment allowlist; never dump the process environment. #>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('before', 'after')][string]$Phase,
    [Parameter(Mandatory)][string]$BuildDir,
    [Parameter(Mandatory)][string]$MetricsPath,
    [Parameter(Mandatory)][string]$CacheKey,
    [Parameter(Mandatory)][ValidateSet('true', 'false', '')][AllowEmptyString()][string]$CacheHit,
    [Parameter(Mandatory)][ValidateSet('msbuild', 'ninja-multi')][string]$Generator
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (-not [IO.Path]::IsPathFullyQualified($BuildDir) -or -not [IO.Path]::IsPathFullyQualified($MetricsPath)) {
    throw 'BuildDir and MetricsPath must be absolute paths'
}

function Tool-Identity([string]$Name, [string[]]$VersionArguments) {
    $tool = Get-Command $Name -CommandType Application -ErrorAction Stop | Select-Object -First 1
    $version = (& $tool.Source @VersionArguments 2>&1 | Out-String).Trim()
    # cl without a source file returns a nonzero code but prints its real version.
    if ([IO.Path]::GetFileName($Name) -ne 'cl.exe' -and $LASTEXITCODE -ne 0) { throw "Cannot inspect $Name" }
    return [ordered]@{ path = $tool.Source; version = $version; sha256 = (Get-FileHash -LiteralPath $tool.Source -Algorithm SHA256).Hash.ToLowerInvariant() }
}

if ($Phase -eq 'before') {
    if (Test-Path -LiteralPath $MetricsPath) { throw 'Environment record already exists' }
    $source = (& git rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read source identity' }
    $tree = (& git rev-parse 'HEAD^{tree}').Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read source tree' }
    $depsRoot = Join-Path (Get-Location) 'deps/build/OrcaSlicer_dep'
    $depFiles = @(Get-ChildItem -LiteralPath $depsRoot -Recurse -File -Filter '*.cmake' |
        Sort-Object FullName | ForEach-Object {
            [ordered]@{ path = [IO.Path]::GetRelativePath($depsRoot, $_.FullName).Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
        })
    if ($depFiles.Count -eq 0) { throw 'Dependency cache contains no CMake configuration files' }
    $cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors)
    $os = Get-CimInstance Win32_OperatingSystem
    $record = [ordered]@{
        schema_version = 1; source_sha = $source; source_tree = $tree; requested_generator = $Generator
        run_id = $env:GITHUB_RUN_ID; run_attempt = $env:GITHUB_RUN_ATTEMPT
        image_os = $env:ImageOS; image_version = $env:ImageVersion
        os_version = $os.Version; cpu = $cpu
        memory_bytes = [long](Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory
        cache_key = $CacheKey; cache_hit = ($CacheHit -eq 'true'); dependency_configs = $depFiles
        vc_tools_version = $env:VCToolsVersion; windows_sdk_version = $env:WindowsSDKVersion
        tools = [ordered]@{
            cmake = (Tool-Identity 'cmake.exe' @('--version'))
            ninja = (Tool-Identity 'ninja.exe' @('--version'))
            msbuild = (Tool-Identity 'MSBuild.exe' @('-version', '-nologo'))
            cl = (Tool-Identity 'cl.exe' @())
        }
        before_utc = [DateTime]::UtcNow.ToString('o'); after_utc = $null; configured = $null; actual_compiler = $null
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $MetricsPath -Parent) | Out-Null
} else {
    $record = Get-Content -LiteralPath $MetricsPath -Raw | ConvertFrom-Json -AsHashtable
    if ($record.source_sha -ne (& git rev-parse HEAD).Trim() -or $record.requested_generator -ne $Generator -or $record.cache_key -ne $CacheKey) {
        throw 'Build environment identity changed during build'
    }
    $values = [ordered]@{}
    $cache = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cache) {
        $allowed = @('CMAKE_GENERATOR', 'CMAKE_GENERATOR_INSTANCE', 'CMAKE_GENERATOR_PLATFORM', 'CMAKE_GENERATOR_TOOLSET', 'CMAKE_CXX_COMPILER', 'CMAKE_C_COMPILER', 'CMAKE_MAKE_PROGRAM', 'CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION', 'CMAKE_BUILD_TYPE', 'CMAKE_CONFIGURATION_TYPES', 'CMAKE_CXX_FLAGS', 'CMAKE_CXX_FLAGS_RELEASE', 'BUILD_TESTS')
        foreach ($line in Get-Content -LiteralPath $cache) {
            if ($line -match '^([^/#][^:]*):[^=]+=(.*)$' -and $Matches[1] -in $allowed) { $values[$Matches[1]] = $Matches[2] }
        }
    }
    $compilerRecords = @(Get-ChildItem -Path (Join-Path $BuildDir 'CMakeFiles/*/CMakeCXXCompiler.cmake') -File -ErrorAction SilentlyContinue)
    foreach ($file in $compilerRecords) {
        foreach ($line in Get-Content -LiteralPath $file.FullName) {
            if ($line -match '^set\((CMAKE_CXX_COMPILER(?:_VERSION|_ID|_FRONTEND_VARIANT)?|CMAKE_CXX_PLATFORM_ID|MSVC_CXX_ARCHITECTURE_ID) "(.*)"\)$') { $values[$Matches[1]] = $Matches[2] }
        }
    }
    if ($values.Contains('CMAKE_CXX_COMPILER')) {
        $record.actual_compiler = Tool-Identity $values.CMAKE_CXX_COMPILER @()
    }
    # The Visual Studio generator's selected SDK can differ from the shell SDK.
    $project = Join-Path $BuildDir 'ALL_BUILD.vcxproj'
    if (Test-Path -LiteralPath $project) {
        $projectText = Get-Content -LiteralPath $project -Raw
        if ($projectText -match '<WindowsTargetPlatformVersion>([^<]+)</WindowsTargetPlatformVersion>') {
            $values['selected_windows_sdk'] = $Matches[1]
        }
        if ($projectText -match '<PlatformToolset>([^<]+)</PlatformToolset>') { $values['selected_platform_toolset'] = $Matches[1] }
    } elseif ($Generator -eq 'ninja-multi') {
        $values['selected_windows_sdk'] = $record.windows_sdk_version.TrimEnd('\', '/')
    }
    $record.configured = $values
    $record.after_utc = [DateTime]::UtcNow.ToString('o')
}
$record | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $MetricsPath -Encoding utf8
# Do not leak cl's intentional version-probe exit code into the calling build step.
$global:LASTEXITCODE = 0
