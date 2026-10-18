<#
.SYNOPSIS
    Launch, skip the intro, optionally drive the console, then close the game.

.DESCRIPTION
    Everything happens in one process with the P/Invoke compiled once, because
    the intro only accepts the ESC skip while the videos are on screen and
    separate PowerShell invocations are far too slow to hit that window.

    The game runs fullscreen at 800x600 and takes over the desktop, so by default
    this closes the game again at the end and reports what happened from the
    mod's log.

.PARAMETER Commands
    Console commands to try, each typed into a freshly opened console.

.PARAMETER KeepOpen
    Leave the game running instead of closing it.

.EXAMPLE
    ./tools/enter-game.ps1
    ./tools/enter-game.ps1 -Commands 'startscene=C0_Training'
#>
[CmdletBinding()]
param(
    [string[]]$Commands = @(),
    [int]$WindowTimeoutSeconds = 120,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Keys.ps1')

# Accept "-Commands a,b,c" as one literal string (that is what -File delivers).
$Commands = @($Commands | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

$logDir = 'E:\SteamLibrary\steamapps\common\Hitman Codename 47\h47vr-logs'

function Read-NewestLog {
    $log = Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $log) { return '' }
    $stream = [System.IO.File]::Open($log.FullName,
        [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $reader = New-Object System.IO.StreamReader($stream)
    $text = $reader.ReadToEnd(); $reader.Dispose(); $stream.Dispose()
    return $text
}

if (Get-GameProcess) { throw 'The game is already running. Close it first.' }

Write-Host 'launching via Steam ...'
Start-Process -FilePath 'C:\Program Files (x86)\Steam\steam.exe' -ArgumentList '-applaunch', '6900'

# Poll for the process, then burst ESC immediately: waiting for a window handle
# misses the skippable part of the intro.
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$deadline = $clock.Elapsed.TotalSeconds + $WindowTimeoutSeconds
$proc = $null
while ($clock.Elapsed.TotalSeconds -lt $deadline) {
    $proc = Get-GameProcess
    if ($proc) { break }
    Start-Sleep -Milliseconds 50
}
if (-not $proc) { throw "No game process appeared within $WindowTimeoutSeconds s." }
Write-Host ("process up after {0:N0} ms (pid {1}); INPUT is {2} bytes" -f
    $clock.Elapsed.TotalMilliseconds, $proc.Id, [H47Input]::StructSize())

# Burst ESC from process start through the window becoming usable, and focus the
# window the moment it exists. Sending ESC before focusing was a real flaw: the
# engine only acts on keys while its own window is active. ESC does nothing once
# the menu is up, so overshooting by a few seconds is harmless.
$burstEnd = $clock.Elapsed.TotalSeconds + 6
$focused = $false
while ($clock.Elapsed.TotalSeconds -lt $burstEnd) {
    if (-not $focused) {
        $candidate = Get-GameProcess
        if ($candidate -and $candidate.MainWindowHandle -ne 0) {
            $focused = Focus-GameWindow -Process $candidate
            if ($focused) {
                Write-Host ("focused at {0:N0} ms: '{1}'" -f
                    $clock.Elapsed.TotalMilliseconds, $candidate.MainWindowTitle)
            }
        }
    }
    [H47Input]::Key(0x1B)
    Start-Sleep -Milliseconds 150
}
Write-Host ("ESC burst done at {0:N0} ms (focused={1})" -f
    $clock.Elapsed.TotalMilliseconds, $focused)

$proc = Get-GameProcess
if (-not $proc) { throw 'the game exited while skipping the intro' }
if (-not $focused) { Write-Warning 'the game window never became foreground' }
Start-Sleep -Seconds 2

foreach ($command in $Commands) {
    Write-Host "console: $command"
    [H47Input]::Key(0xC0)                      # backtick
    Start-Sleep -Milliseconds 500
    [H47Input]::Text($command)
    Start-Sleep -Milliseconds 300
    [H47Input]::Key(0x0D)                      # enter
    Start-Sleep -Seconds 5

    $text = Read-NewestLog
    $counters = @($text -split "`n" | Where-Object { $_ -match 'renderer calls' } | Select-Object -Last 1)
    $engine = @($text -split "`n" | Where-Object { $_ -match 'engine:' })
    Write-Host ("  alive={0}  engine lines={1}" -f
        [bool](Get-GameProcess), $engine.Count)
    if ($counters.Count -gt 0) { Write-Host ("  {0}" -f $counters[0].Trim()) }
    $engine | Select-Object -Last 5

    if (-not (Get-GameProcess)) {
        Write-Host '  game exited after the command'
        break
    }
}

$text = Read-NewestLog
$engine = @($text -split "`n" | Where-Object { $_ -match 'engine:' })
Write-Host ("engine lines captured in total: {0}" -f $engine.Count)

if (-not $KeepOpen) {
    Get-GameProcess | ForEach-Object { $_.Kill() }
    Write-Host 'game closed.'
}
