#Requires -Version 5.1
<#
.SYNOPSIS
    Exercise the wide-FOV projection and candidate culling paths without a headset.

.DESCRIPTION
    Writes <game>\h47vr-widefov.txt. During gameplay the FOV is passed into both
    the engine's own camera projection/visibility builder and the desktop SBS
    renderer, so the diagnostic exercises culling and final rendering together.
    -NoClipBypass keeps the native polygon path; without it, the candidate
    0x93670 bypass is also enabled for comparison. The bypass is unproven and
    this is a diagnostic, not a normal play mode.
#>
[CmdletBinding()]
param(
    [ValidateRange(35.0, 140.0)]
    [double]$Degrees = 122.0,
    [switch]$NoClipBypass,
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
            ForEach-Object { $_.Matches } |
            ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        foreach ($root in $roots) {
            $candidate = Join-Path $root 'steamapps/common/Hitman Codename 47'
            if (Test-Path -LiteralPath (Join-Path $candidate 'Hitman.Exe')) {
                return $candidate
            }
        }
    }
    throw 'Could not locate the game. Pass -GameDir explicitly.'
}

$GameDir = Resolve-GameDir -Requested $GameDir
$gate = Join-Path $GameDir 'h47vr-widefov.txt'

if ($Off) {
    Remove-Item -LiteralPath $gate -Force -ErrorAction SilentlyContinue
    Write-Host 'wide-FOV diagnostic off'
    exit 0
}

$clip = if ($NoClipBypass) { 0 } else { 1 }
Set-Content -LiteralPath $gate -Value ("fov_deg={0} clip={1}" -f $Degrees, $clip) `
    -Encoding ascii
Write-Host ("wide-FOV diagnostic on: {0} degrees horizontal -> {1}" -f
    $Degrees, $gate)
