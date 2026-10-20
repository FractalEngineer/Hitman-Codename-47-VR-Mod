#Requires -Version 5.1
<#
.SYNOPSIS
    Enable native engine pitch for head-pose testing.

.DESCRIPTION
    Writes <game>\h47vr-nativepitch.txt. When present, headset pitch is fed to
    the same MouseConCam accumulator used by yaw so the engine's culling
    camera rotates with the submitted view. Remove it to return to the stable
    GL-seam pitch fallback.
#>
[CmdletBinding()]
param(
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
$control = Join-Path $GameDir 'h47vr-nativepitch.txt'
if ($Off) {
    Remove-Item -LiteralPath $control -Force -ErrorAction SilentlyContinue
    Write-Host 'native MouseConCam pitch off'
} else {
    Set-Content -LiteralPath $control -Value 'on' -Encoding ascii
    Write-Host 'native MouseConCam pitch on'
}
