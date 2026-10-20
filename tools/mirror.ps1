#Requires -Version 5.1
<#
.SYNOPSIS
    Mirror the submitted OpenXR eye image into the game window.

.DESCRIPTION
    Writes <game>\h47vr-mirror.txt. When direct per-eye rendering is active,
    the renderer blits the completed runtime swapchain image back to the
    desktop window before SwapBuffers. Left and right preserve the headset
    image's aspect; side-by-side uses both eye images. The same projection,
    visibility state and sRGB swapchain are used for both outputs.

.PARAMETER Layout
    Left, Right, Sbs, or Off.

.PARAMETER Capture
    Ask the running renderer to dump the next mirrored XR frame to
    <game>\h47vr-mirror-capture.bmp.
#>
[CmdletBinding()]
param(
    [ValidateSet('Left', 'Right', 'Sbs', 'Off')]
    [string]$Layout = 'Sbs',
    [switch]$Capture,
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
$control = Join-Path $GameDir 'h47vr-mirror.txt'
if ($Layout -eq 'Off') {
    Remove-Item -LiteralPath $control -Force -ErrorAction SilentlyContinue
    Write-Host 'XR desktop mirror off'
} else {
    $value = $Layout.ToLowerInvariant()
    Set-Content -LiteralPath $control -Value ("layout={0}" -f $value) -Encoding ascii
    Write-Host ("XR desktop mirror: {0}" -f $value)
}

if ($Capture) {
    $trigger = Join-Path $GameDir 'h47vr-mirror-capture.txt'
    $output = Join-Path $GameDir 'h47vr-mirror-capture.bmp'
    Remove-Item -LiteralPath $output -Force -ErrorAction SilentlyContinue
    Set-Content -LiteralPath $trigger -Value 'capture' -Encoding ascii
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline -and
           -not (Test-Path -LiteralPath $output)) {
        Start-Sleep -Milliseconds 100
    }
    if (Test-Path -LiteralPath $output) {
        Write-Host ("captured mirrored XR frame: {0}" -f $output)
    } else {
        throw 'the renderer did not produce a mirror capture; is direct stereo active?'
    }
}
