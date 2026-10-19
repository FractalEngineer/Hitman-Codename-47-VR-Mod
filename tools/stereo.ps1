#Requires -Version 5.1
<#
.SYNOPSIS
    Toggle the side-by-side stereo prototype in the running game.

.DESCRIPTION
    Writes <game>\h47vr-stereo.txt while stereo is on. The renderer duplicates
    perspective draw-list passes into left and right half-viewports. Orthographic
    UI and menu passes remain single. Eye separation is in game units; the
    training-level camera offset suggests centimetres, so 10 is a useful first
    test value.

.PARAMETER Ipd
    Eye separation in game units. Defaults to 10.

.PARAMETER Off
    Remove the control file and return to normal rendering.
#>
[CmdletBinding()]
param(
    [double]$Ipd = 10.0,
    [switch]$Off,
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'

function Resolve-GameDir {
    param([string]$Requested)
    if ($Requested) { return (Resolve-Path -LiteralPath $Requested).Path }
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
    throw 'Could not locate the game. Pass -GameDir explicitly.'
}

$GameDir = Resolve-GameDir -Requested $GameDir
$control = Join-Path $GameDir 'h47vr-stereo.txt'

if ($Off) {
    Remove-Item -LiteralPath $control -Force -ErrorAction SilentlyContinue
    Write-Host 'stereo off'
    exit 0
}

if ($Ipd -le 0 -or $Ipd -gt 1000) {
    throw 'Ipd must be between 0 and 1000 game units.'
}

Set-Content -LiteralPath $control -Value ("ipd={0}" -f $Ipd) -Encoding ascii
Write-Host ("stereo on: {0}" -f $control)
