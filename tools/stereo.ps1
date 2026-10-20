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
    projection scale. Immersive duplication is active only while MouseConCam
    updates recently; the intro, menus and scripted cameras use one theater
    pass. Current mode is published in h47vr-stereo-state.txt.

.PARAMETER IpdMillimeters
    Physical eye separation in millimetres. Defaults to 67.

.PARAMETER GameUnitsPerMetre
    World-space scale. Defaults to 100, based on the 150-unit follow camera.

.PARAMETER CameraLower
    Vertical camera-origin correction in game units. Positive values lower the
    engine camera after MouseConCam updates it, so collision, visibility and the
    pitch pivot use the same corrected position.

.PARAMETER CameraForward
    Forward eye-alignment correction in game units. Positive values move the
    physical camera toward the facing direction without changing the engine's
    follow distance or collision path.

.PARAMETER HeadClipNear
    Gameplay-only near-plane distance in game units. The default 24-unit cut
    removes the player's head geometry around the first-person camera while
    leaving the body and shadow passes intact. The normal near plane returns as
    soon as a cinematic or scripted camera moves the head out of range.

.PARAMETER Ipd
    Legacy override in raw game units. Use only for calibration experiments.

.PARAMETER Off
    Remove the control file and return to normal rendering.
#>
[CmdletBinding()]
param(
    [double]$IpdMillimeters = 67.0,
    [double]$GameUnitsPerMetre = 100.0,
    [double]$CameraLower = 0.0,
    [double]$CameraForward = 2.2,
    [double]$HeadClipNear = 24.0,
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
if ($CameraLower -lt -100 -or $CameraLower -gt 100) {
    throw 'CameraLower must be between -100 and 100 game units.'
}
if ($CameraForward -lt -100 -or $CameraForward -gt 100) {
    throw 'CameraForward must be between -100 and 100 game units.'
}
if ($HeadClipNear -lt 0 -or $HeadClipNear -gt 1000) {
    throw 'HeadClipNear must be between 0 and 1000 game units.'
}
$request = if ($PSBoundParameters.ContainsKey('Ipd')) {
    "ipd={0}" -f $Ipd
} else {
    "ipd_mm={0} units_per_m={1} camera_lower={2} camera_forward={3} head_near={4}" -f `
        $IpdMillimeters, $GameUnitsPerMetre, $CameraLower, $CameraForward, $HeadClipNear
}
Set-Content -LiteralPath $control -Value $request -Encoding ascii
Write-Host ("stereo on: {0}" -f $control)
