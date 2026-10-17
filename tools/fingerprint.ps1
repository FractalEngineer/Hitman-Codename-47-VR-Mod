<#
.SYNOPSIS
    Verify a Hitman: Codename 47 install against the supported build pins.

.DESCRIPTION
    Reads pins/build_251800.txt and compares SHA-256 of every listed module in
    the target game directory. Exits 0 only when the whole install matches the
    supported build. Any mismatch or missing file exits 1, matching the mod's
    refusal to patch unknown builds.

.PARAMETER GameDir
    Game directory to check. Defaults to the .gamedev copy when one exists,
    otherwise the Steam install discovered from libraryfolders.vdf.

.PARAMETER Pins
    Pin file. Defaults to pins/build_251800.txt in this repository.

.PARAMETER Json
    Emit a machine-readable report instead of a table.

.EXAMPLE
    ./tools/fingerprint.ps1
    ./tools/fingerprint.ps1 -GameDir ./.gamedev/Hitman -Json
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Pins,
    [switch]$Json
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $Pins) { $Pins = Join-Path $repoRoot 'pins/build_251800.txt' }
if (-not (Test-Path -LiteralPath $Pins)) { throw "Pin file not found: $Pins" }

function Find-GameDir {
    $devCopy = Join-Path $repoRoot '.gamedev/Hitman'
    if (Test-Path -LiteralPath (Join-Path $devCopy 'Hitman.Exe')) { return $devCopy }

    $vdfs = @(
        'C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf',
        'C:\Program Files\Steam\steamapps\libraryfolders.vdf'
    )
    foreach ($vdf in $vdfs) {
        if (-not (Test-Path -LiteralPath $vdf)) { continue }
        $roots = Select-String -LiteralPath $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches |
            ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        foreach ($root in $roots) {
            $candidate = Join-Path $root 'steamapps/common/Hitman Codename 47'
            if (Test-Path -LiteralPath (Join-Path $candidate 'Hitman.Exe')) { return $candidate }
        }
    }
    return $null
}

if (-not $GameDir) {
    $GameDir = Find-GameDir
    if (-not $GameDir) { throw 'Could not locate the game. Pass -GameDir explicitly.' }
}
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path

# --- parse pins -------------------------------------------------------------
$appId = $null
$buildId = $null
$modules = [System.Collections.Generic.List[object]]::new()

foreach ($line in Get-Content -LiteralPath $Pins) {
    $trimmed = $line.Trim()
    if ($trimmed -eq '' -or $trimmed.StartsWith('#')) { continue }
    $parts = $trimmed -split '\s+'
    switch ($parts[0]) {
        'appid'   { $appId = $parts[1] }
        'buildid' { $buildId = $parts[1] }
        'module'  {
            if ($parts.Count -lt 3) { throw "Malformed module pin: $line" }
            $modules.Add([pscustomobject]@{
                Path     = ($parts[1] -replace '/', '\')
                Expected = $parts[2].ToUpperInvariant()
            })
        }
        default { throw "Unknown pin key '$($parts[0])' in: $line" }
    }
}

# --- verify -----------------------------------------------------------------
$results = foreach ($m in $modules) {
    $full = Join-Path $GameDir $m.Path
    $present = Test-Path -LiteralPath $full
    $actual = if ($present) { (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash } else { $null }
    [pscustomobject]@{
        Path     = $m.Path
        Present  = $present
        Expected = $m.Expected
        Actual   = $actual
        Ok       = ($present -and $actual -eq $m.Expected)
    }
}

$missing = @($results | Where-Object { -not $_.Present })
$mismatched = @($results | Where-Object { $_.Present -and -not $_.Ok })
$allOk = ($missing.Count -eq 0 -and $mismatched.Count -eq 0)

if ($Json) {
    [pscustomobject]@{
        gameDir = $GameDir
        appId   = $appId
        buildId = $buildId
        allOk   = $allOk
        results = $results
    } | ConvertTo-Json -Depth 4
} else {
    Write-Host "Game directory : $GameDir"
    Write-Host "Expected build : appid $appId buildid $buildId"
    Write-Host "Pin file       : $Pins"
    Write-Host ''
    foreach ($r in $results) {
        $mark = if ($r.Ok) { 'OK  ' } elseif (-not $r.Present) { 'MISS' } else { 'FAIL' }
        Write-Host ("  [{0}] {1}" -f $mark, $r.Path)
        if ($r.Present -and -not $r.Ok) {
            Write-Host ("         expected {0}" -f $r.Expected)
            Write-Host ("         actual   {0}" -f $r.Actual)
        }
    }
    Write-Host ''
    if ($allOk) {
        Write-Host 'Result: MATCH - this install is a supported build.' -ForegroundColor Green
    } else {
        Write-Host ("Result: UNSUPPORTED - {0} missing, {1} mismatched." -f $missing.Count, $mismatched.Count) -ForegroundColor Red
        Write-Host 'The mod refuses to patch unknown builds. Do not report bugs against this install.'
    }
}

if ($allOk) { exit 0 } else { exit 1 }
