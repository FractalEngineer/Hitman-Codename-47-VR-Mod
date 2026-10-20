#Requires -Version 5.1
<#
.SYNOPSIS
    Enable or disable the experimental first-person gameplay camera.

.DESCRIPTION
    Writes <game>\h47vr-firstperson.txt. While present, MouseConCam's normal
    150-unit follow distance (100 while leaning) is replaced with the requested
    game-unit distance. The engine still performs its own camera transform,
    visibility pass and collision checks.

    This is a camera-position experiment, not yet a complete first-person mode:
    the player's head still needs to be hidden, and eye height/near clipping
    still need calibration.

.PARAMETER Distance
    Follow-camera distance in game units. At the working 110 units/metre scale,
    the 3-unit default is about 2.7 cm. Valid range is 0.25 to 149.

.PARAMETER Off
    Remove the control file and restore the original follow distance.
#>
[CmdletBinding()]
param(
    [double]$Distance = 3.0,
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
$control = Join-Path $GameDir 'h47vr-firstperson.txt'

if ($Off) {
    Remove-Item -LiteralPath $control -Force -ErrorAction SilentlyContinue
    Write-Host 'first person off'
    exit 0
}

if ($Distance -lt 0.25 -or $Distance -ge 149.0) {
    throw 'Distance must be between 0.25 and 149 game units.'
}

Set-Content -LiteralPath $control -Value ("distance={0}" -f $Distance) -Encoding ascii
Write-Host ("first person camera on: distance {0} game units -> {1}" -f $Distance, $control)
