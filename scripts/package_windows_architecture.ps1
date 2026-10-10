function Resolve-PackageWindowsArchitecture {
    param([Parameter(Mandatory = $true)][string] $BuildDir)

    $cacheText = Get-Content -LiteralPath (Join-Path $BuildDir 'CMakeCache.txt') -Raw
    $platformMatch = [regex]::Match($cacheText, '(?m)^CMAKE_GENERATOR_PLATFORM:[^=]+=([^\r\n]*)')
    $platform = if ($platformMatch.Success) { $platformMatch.Groups[1].Value.Trim() } else { '' }
    $compilerArchitecture = ''
    $versionParts = foreach ($part in @('MAJOR', 'MINOR', 'PATCH')) {
        $match = [regex]::Match($cacheText, "(?m)^CMAKE_CACHE_${part}_VERSION:[^=]+=(\d+)\s*$")
        if ($match.Success) { $match.Groups[1].Value }
    }
    if (@($versionParts).Count -eq 3) {
        $compilerPath = Join-Path $BuildDir "CMakeFiles/$($versionParts -join '.')/CMakeCXXCompiler.cmake"
        if (Test-Path -LiteralPath $compilerPath -PathType Leaf) {
            $compilerText = Get-Content -LiteralPath $compilerPath -Raw
            $compilerMatch = [regex]::Match($compilerText, '(?m)^set\(CMAKE_CXX_COMPILER_ARCHITECTURE_ID\s+"?([A-Za-z0-9_]+)"?\)\s*$')
            if ($compilerMatch.Success) { $compilerArchitecture = $compilerMatch.Groups[1].Value }
        }
    }

    # An empty Visual Studio platform means its default target. Read that target
    # from CMake's compiler record, never infer it from the host processor.
    $configuredArchitecture = if ($platform) { $platform } else { $compilerArchitecture }
    if ($configuredArchitecture -notmatch '^(?i:x64|amd64|x86_64|arm64|aarch64)$') {
        throw "Unsupported or missing Windows package architecture: '$configuredArchitecture'."
    }
    $architecture = if ($configuredArchitecture -match '^(?i:arm64|aarch64)$') { 'arm64' } else { 'x64' }
    if ($platform -and $compilerArchitecture) {
        $compiledArchitecture = if ($compilerArchitecture -match '^(?i:arm64|aarch64)$') { 'arm64' }
            elseif ($compilerArchitecture -match '^(?i:x64|amd64|x86_64)$') { 'x64' } else { '' }
        if ($compiledArchitecture -ne $architecture) {
            throw 'Windows package architecture conflicts with the configured compiler target.'
        }
    }
    return $architecture
}
