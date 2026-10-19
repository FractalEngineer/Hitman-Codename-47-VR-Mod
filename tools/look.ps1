#Requires -Version 5.1
<#
.SYNOPSIS
    Turn the game's own camera from inside the game, with no focus and no
    synthetic input.

DESCRIPTION
    Writes <game>\h47vr-look.txt. The mod polls for it on the engine's frame
    boundary, posts WM_MOUSEMOVE messages to the game's own window, and deletes
    the file again - so one write is one turn.

    This replaces the SetForegroundWindow + SendInput route in turn.ps1. The
    engine reads the mouse only from window messages (there is no GetCursorPos
    and no DirectInput import anywhere in the game), so a posted message is
    exactly what a real mouse move delivers, without needing the window to be
    in the foreground. It also means the game keeps its own desktop focus and
    the tooling shell can stay usable while the turn happens.

.PARAMETER Dx
    Horizontal movement in pixels. Positive turns right.

.PARAMETER Dy
    Vertical movement in pixels. Positive looks down.

.PARAMETER Steps
    How many frames to spread the turn over. The engine discards a jump.

.PARAMETER Yaw
    Absolute target yaw in degrees, relative to where the driver's estimate
    started. Mutually usable with -Pitch; either alone leaves the other where it
    is.

.PARAMETER DegPerPixel
    Camera degrees per mouse pixel, for absolute requests. The default is
    0.1289156, measured against the engine's live camera readback in the
    training level. The mod ignores this fallback while readback is available.

.PARAMETER Verify
    Capture before and after with tools/capture.ps1 and report the difference.

.EXAMPLE
    ./tools/look.ps1 -Dx 220 -Verify
    ./tools/look.ps1 -Dy -120
    ./tools/look.ps1 -Yaw 90 -Verify
#>
[CmdletBinding()]
param(
    [int]$Dx = 0,
    [int]$Dy = 0,
    [double]$Yaw,
    [double]$Pitch,
    [double]$DegPerPixel,
    [int]$Steps = 15,
    [int]$Rate = 1,
    [string]$GameDir,
    [string]$ProcessName = 'Hitman',
    [switch]$Verify
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

$game = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $game) { throw "The game is not running. Start it first; the mod posts the move in-process." }

$shots = Join-Path $PSScriptRoot '_shots'
if (-not (Test-Path -LiteralPath $shots)) { New-Item -ItemType Directory -Path $shots | Out-Null }
$before = Join-Path $shots 'look_before.png'
if ($Verify) {
    & (Join-Path $PSScriptRoot 'capture.ps1') -Path $before | Out-Null
}

$lookFile = Join-Path $GameDir 'h47vr-look.txt'
$parts = @()
if ($PSBoundParameters.ContainsKey('Yaw')) { $parts += "yaw=$Yaw" }
if ($PSBoundParameters.ContainsKey('Pitch')) { $parts += "pitch=$Pitch" }
if ($PSBoundParameters.ContainsKey('DegPerPixel')) { $parts += "degperpx=$DegPerPixel" }
if ($parts.Count -eq 0) { $parts += "dx=$Dx"; $parts += "dy=$Dy" }
$parts += "steps=$Steps"
$parts += "rate=$Rate"
$request = $parts -join ' '
Set-Content -LiteralPath $lookFile -Value $request -Encoding ascii
Write-Host ("request written: {0} -> '{1}'" -f $lookFile, $request)

# The mod consumes the file on sight; wait for -Steps frames' worth of time.
$waitMs = [Math]::Max(600, ($Steps * $Rate * 40))
$deadline = (Get-Date).AddMilliseconds($waitMs)
while ((Test-Path -LiteralPath $lookFile) -and (Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 100
}
if (Test-Path -LiteralPath $lookFile) {
    Write-Warning 'the mod has not consumed the request yet; is it loaded and in a level?'
}
Start-Sleep -Milliseconds 400

$logDir = Join-Path $GameDir 'h47vr-logs'
$log = Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) {
    $text = Get-Content -LiteralPath $log.FullName -Tail 200 -ErrorAction SilentlyContinue
    $lines = @($text | Where-Object { $_ -match 'look driver' })
    if ($lines.Count -gt 0) {
        Write-Host ''
        Write-Host ("--- {0}, look driver lines ---" -f $log.Name)
        $lines | Select-Object -Last 6 | ForEach-Object { Write-Host ("  {0}" -f $_.TrimEnd()) }
    }
}

$stateFile = Join-Path $GameDir 'h47vr-lookstate.txt'
if (Test-Path -LiteralPath $stateFile) {
    Write-Host ("  aim estimate: {0}" -f (Get-Content -LiteralPath $stateFile -Raw).Trim())
}

if ($Verify) {
    $after = Join-Path $shots 'look_after.png'
    & (Join-Path $PSScriptRoot 'capture.ps1') -Path $after | Out-Null
    & (Join-Path $PSScriptRoot 'capture.ps1') -Path $before -Compare $after
}
