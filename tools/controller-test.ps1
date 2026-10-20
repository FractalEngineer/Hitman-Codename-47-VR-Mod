#Requires -Version 5.1
<#
.SYNOPSIS
    Inject a controller sample for headless input verification.

.DESCRIPTION
    Writes <game>\h47vr-controller-test.txt. The controller adapter reads this
    sample instead of the live OpenXR actions, letting the engine keyboard-state
    mapping and native turn path be checked without connected controllers.

.PARAMETER Off
    Remove the test sample and return to live OpenXR input.
#>
[CmdletBinding()]
param(
    [double]$LeftX = 0.0,
    [double]$LeftY = 0.0,
    [double]$RightX = 0.0,
    [double]$RightY = 0.0,
    [switch]$Escape,
    [switch]$Snap,
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
$control = Join-Path $GameDir 'h47vr-controller-test.txt'
if ($Off) {
    Remove-Item -LiteralPath $control -Force -ErrorAction SilentlyContinue
    Write-Host 'controller test override off'
} else {
    Set-Content -LiteralPath $control -Encoding ascii -Value (
        'lx={0} ly={1} rx={2} ry={3} esc={4} snap={5}' -f
            $LeftX, $LeftY, $RightX, $RightY,
            $(if ($Escape) { 1 } else { 0 }),
            $(if ($Snap) { 1 } else { 0 }))
    Write-Host 'controller test sample written'
}
