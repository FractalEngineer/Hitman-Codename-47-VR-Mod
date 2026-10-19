#Requires -Version 5.1
<#
.SYNOPSIS
    Enable or disable the OpenXR pose-only backend in the running game.

.DESCRIPTION
    The pose backend is gated by <game>\h47vr-openxr.txt so normal desktop play
    never initializes a headset runtime. When enabled, it creates an OpenXR
    session on the game's current WGL context and feeds predicted eye positions,
    asymmetric FOV tangents and head orientation into the renderer. This is
    still a diagnostic step; it does not submit game frames to a headset.
#>
[CmdletBinding()]
param(
    [switch]$Enable,
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
$gate = Join-Path $GameDir 'h47vr-openxr.txt'

if ($Off -or -not $Enable) {
    Remove-Item -LiteralPath $gate -Force -ErrorAction SilentlyContinue
    Write-Host 'OpenXR pose backend disabled'
} else {
    Set-Content -LiteralPath $gate -Value 'on' -Encoding ascii
    Write-Host ("OpenXR pose backend enabled: {0}" -f $gate)
}
