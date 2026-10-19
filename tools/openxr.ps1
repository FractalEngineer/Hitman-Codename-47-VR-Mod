#Requires -Version 5.1
<#
.SYNOPSIS
    Enable or disable the OpenXR pose and presentation backend in the running game.

.DESCRIPTION
    The pose backend is gated by <game>\h47vr-openxr.txt so normal desktop play
    never initializes a headset runtime. When enabled, it creates an OpenXR
    session on the game's current WGL context and feeds predicted eye positions,
    asymmetric FOV tangents and head orientation into the renderer. This is
    still a diagnostic step; presentation uses the runtime's accepted OpenGL
    swapchains for theater and gameplay layers. A physical-headset quality pass
    remains.

.PARAMETER RuntimeJson
    Optional OpenXR runtime manifest for this test only. The mod sets
    XR_RUNTIME_JSON inside the game process before creating its instance, so the
    machine's active runtime registry is left untouched.
#>
[CmdletBinding()]
param(
    [switch]$Enable,
    [switch]$Off,
    [string]$RuntimeJson,
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
$runtimeFile = Join-Path $GameDir 'h47vr-openxr-runtime.txt'

if ($Off -or (-not $Enable -and -not $RuntimeJson)) {
    Remove-Item -LiteralPath $gate -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $runtimeFile -Force -ErrorAction SilentlyContinue
    Write-Host 'OpenXR backend disabled'
} else {
    Set-Content -LiteralPath $gate -Value 'on' -Encoding ascii
    if ($RuntimeJson) {
        $resolved = (Resolve-Path -LiteralPath $RuntimeJson).Path
        Set-Content -LiteralPath $runtimeFile -Value $resolved -Encoding utf8
        Write-Host ("OpenXR runtime override: {0}" -f $resolved)
    } else {
        Remove-Item -LiteralPath $runtimeFile -Force -ErrorAction SilentlyContinue
    }
    Write-Host ("OpenXR backend enabled: {0}" -f $gate)
}
