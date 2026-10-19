#Requires -Version 5.1
<#
.SYNOPSIS
    Turn the game's own camera with synthetic mouse movement.

.DESCRIPTION
    The engine reads the mouse as deltas and rotates its own camera from them.
    Driving it that way controls the view without touching any engine code:
    because the engine does the rotating, its visibility pass and its geometry
    transform both follow, and the world stays whole. Rotating the geometry at
    the GL boundary cannot do that - the engine has already thrown away
    everything outside the frustum it believes in.

    Focus has to be taken first, and a plain SetForegroundWindow is refused;
    Keys.ps1's Focus-GameWindow uses the AttachThreadInput route, which works.

.PARAMETER Dx
    Horizontal movement in pixels. Positive turns right.

.PARAMETER Dy
    Vertical movement in pixels. Positive looks down.

.PARAMETER Steps
    How many small movements to split it into. The game discards one big jump.

.PARAMETER Verify
    Capture before and after with tools/capture.ps1 and report the difference.

.EXAMPLE
    ./tools/turn.ps1 -Dx 220 -Verify
#>
[CmdletBinding()]
param(
    [int]$Dx = 0,
    [int]$Dy = 0,
    [int]$Steps = 15,
    [string]$ProcessName = 'Hitman',
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Keys.ps1')

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "no $ProcessName window found" }

if (-not (Focus-GameWindow -Process $process)) {
    Write-Warning 'the game window is not foreground; the movement may be discarded'
}

$shots = Join-Path $PSScriptRoot '_shots'
$before = Join-Path $shots 'turn_before.png'
if ($Verify) {
    & (Join-Path $PSScriptRoot 'capture.ps1') -Path $before | Out-Null
}

Move-GameMouse -Dx $Dx -Dy $Dy -Steps $Steps
Write-Host "moved the mouse by ($Dx, $Dy) in $Steps steps"

if ($Verify) {
    $after = Join-Path $shots 'turn_after.png'
    & (Join-Path $PSScriptRoot 'capture.ps1') -Path $after | Out-Null
    & (Join-Path $PSScriptRoot 'capture.ps1') -Path $before -Compare $after
}
