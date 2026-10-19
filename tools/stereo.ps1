#Requires -Version 5.1
<#
.SYNOPSIS
    Toggle the side-by-side stereo prototype in the running game.

.DESCRIPTION
    Writes <game>\h47vr-stereo.txt while stereo is on. The renderer duplicates
    perspective draw-list passes into left and right half-viewports. Orthographic
    UI and menu passes remain single. Eye separation is in game units; the
    training-level camera offset suggests 100 game units per metre, so the
    default physical separation is the user's 67 mm IPD. When OpenXR provides
    valid views, their per-eye FOV tangents replace the desktop half-viewport
    projection scale.

.PARAMETER IpdMillimeters
    Physical eye separation in millimetres. Defaults to 67.

.PARAMETER GameUnitsPerMetre
    World-space scale. Defaults to 100, based on the 150-unit follow camera.

.PARAMETER Ipd
    Legacy override in raw game units. Use only for calibration experiments.

.PARAMETER Off
    Remove the control file and return to normal rendering.
#>
[CmdletBinding()]
param(
    [double]$IpdMillimeters = 67.0,
    [double]$GameUnitsPerMetre = 100.0,
    [double]$Ipd,
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

if ($IpdMillimeters -le 0 -or $IpdMillimeters -gt 1000) {
    throw 'IpdMillimeters must be between 0 and 1000.'
}

$request = if ($PSBoundParameters.ContainsKey('Ipd')) {
    "ipd={0}" -f $Ipd
} else {
    "ipd_mm={0} units_per_m={1}" -f $IpdMillimeters, $GameUnitsPerMetre
}
Set-Content -LiteralPath $control -Value $request -Encoding ascii
Write-Host ("stereo on: {0}" -f $control)
