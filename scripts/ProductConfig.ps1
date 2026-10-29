function Get-ProductIdentity {
    param([string]$Root = (Split-Path $PSScriptRoot -Parent))
    $product = Get-Content -LiteralPath (Join-Path $Root 'config/product.json') -Raw | ConvertFrom-Json
    if ($product.display_name -isnot [string] -or $product.version -isnot [string] -or
        $product.display_name -cnotmatch '\A[A-Za-z0-9]+( [A-Za-z0-9]+)*\z' -or
        $product.version -cnotmatch '\A(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\z' -or
        ($product.build_number -isnot [int] -and $product.build_number -isnot [long]) -or
        $product.build_number -lt 0 -or $product.build_number -gt 65535) {
        throw 'Invalid config/product.json name, version or build_number.'
    }
    foreach ($part in $product.version.Split('.')) {
        if ([double]$part -gt 65535) { throw 'Product version components must be between 0 and 65535.' }
    }
    return [pscustomobject]@{
        Name = $product.display_name; Version = $product.version
        DisplayVersion = "V$($product.version)"; FileVersion = "$($product.version).$($product.build_number)"
        Exe = "$($product.display_name).exe"; Dll = "$($product.display_name).dll"; Pdb = "$($product.display_name).pdb"
        PackageStem = $product.display_name.Replace(' ', '_')
        Command = $product.display_name.ToLowerInvariant().Replace(' ', '-')
    }
}
