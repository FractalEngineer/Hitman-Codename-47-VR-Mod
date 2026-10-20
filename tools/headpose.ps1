#Requires -Version 5.1
<#
.SYNOPSIS
    Drive the native head-yaw path without a physical headset.

.DESCRIPTION
    Writes <game>\h47vr-headpose.txt. The look driver treats the requested yaw
    and pitch as the current head pose, so the same native MouseConCam yaw
    accumulator can be verified under a null-HMD runtime.

.PARAMETER Yaw
    Head-relative yaw in degrees.

.PARAMETER Pitch
    Head-relative pitch in degrees, positive up.

.PARAMETER Off
    Remove the override.
#>
[CmdletBinding()]
param(
    [double]$Yaw = 0.0,
    [double]$Pitch = 0.0,
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
$control = Join-Path $GameDir 'h47vr-headpose.txt'
if ($Off) {
    Remove-Item -LiteralPath $control -Force -ErrorAction SilentlyContinue
    Write-Host 'head pose test override off'
} else {
    Set-Content -LiteralPath $control -Encoding ascii `
        -Value ("yaw={0} pitch={1}" -f $Yaw, $Pitch)
    Write-Host ("head pose test override: yaw={0} pitch={1}" -f $Yaw, $Pitch)
}
