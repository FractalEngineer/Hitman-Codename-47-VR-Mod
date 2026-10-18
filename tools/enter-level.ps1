<#
.SYNOPSIS
    Launch, skip the intro, and ask the mod to load a scene - in one go.

.DESCRIPTION
    This is the whole loop in the order it has to happen:

      1. make sure the game is running (launch via Steam if not)
      2. wait a moment so the videos are actually on screen
      3. press ESC for a couple of seconds to skip them, then STOP
      4. write <game>\h47vr-go.txt, which the mod turns into
         `startscene <scene>` on its own thread
      5. watch the log for the result

    Step 3 has to be short. The videos need ESC, the front end does not, and a
    long burst keeps pressing ESC into the menu, where it navigates back out.
    That is why an earlier 25-second burst looked like it skipped the intro and
    then killed the game.

    The keys carry hardware scan codes now (see tools/Keys.ps1): this game reads
    the keyboard through DirectInput, which ignores an event that only carries a
    virtual key, which is why earlier bursts did nothing at all.

.PARAMETER Scene
    Scene to load. Relative path, no extension, from the game's own level-select
    table: 'C0_Training\C0_1'.

.EXAMPLE
    ./tools/enter-level.ps1 -Scene 'C0_Training\C0_1'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Scene,
    [string]$GameDir,
    [int]$StartDelaySeconds = 3,
    [int]$BurstSeconds = 3,
    [int]$SettleSeconds = 3,
    [int]$WatchSeconds = 30,
    [switch]$KeepOpen
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Keys.ps1')

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

function Read-SharedText([string]$Path) {
    try {
        $stream = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $reader = New-Object System.IO.StreamReader($stream)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
    } catch { return $null }
}

$GameDir = Resolve-GameDir -Requested $GameDir
$logDir = Join-Path $GameDir 'h47vr-logs'
$goFile = Join-Path $GameDir 'h47vr-go.txt'

if (Test-Path -LiteralPath $goFile) { Remove-Item -LiteralPath $goFile -Force }

$game = Get-GameProcess
if (-not $game) {
    Write-Host 'launching via Steam ...'
    Start-Process -FilePath 'C:\Program Files (x86)\Steam\steam.exe' -ArgumentList '-applaunch', '6900'
    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 50
        $game = Get-GameProcess
        if ($game) { break }
    }
    if (-not $game) { throw 'No game process appeared within 60 s.' }
    $freshlyLaunched = $true
} else {
    Write-Host 'game already running; reusing it'
    $freshlyLaunched = $false
}

$clock = [System.Diagnostics.Stopwatch]::StartNew()
if ($freshlyLaunched) {
    # The videos do not start until a few seconds in, so pressing ESC at once
    # does nothing. Wait for the window to exist, then a moment longer, so the
    # burst lands while the videos are actually playing.
    while ($clock.Elapsed.TotalSeconds -lt $StartDelaySeconds) {
        Start-Sleep -Milliseconds 100
    }
}

Write-Host ("skipping the intro: ESC for {0}s" -f $BurstSeconds)
$burstEnd = $clock.Elapsed.TotalSeconds + $BurstSeconds
while ($clock.Elapsed.TotalSeconds -lt $burstEnd) {
    $current = Get-GameProcess
    if (-not $current) { throw 'the game exited while the intro was being skipped' }
    if ($current.MainWindowHandle -ne 0) { [void](Focus-GameWindow -Process $current) }
    [H47Input]::Key(0x1B)
    Start-Sleep -Milliseconds 150
}

Start-Sleep -Seconds $SettleSeconds
if (-not (Get-GameProcess)) { throw 'the game exited before the scene request could be sent' }

Set-Content -LiteralPath $goFile -Value $Scene -Encoding ascii
Write-Host ("scene request written: '{0}'" -f $Scene)

for ($i = 0; $i -lt ($WatchSeconds * 2); $i++) {
    Start-Sleep -Milliseconds 500
    if (-not (Test-Path -LiteralPath $goFile)) { break }
}
Start-Sleep -Milliseconds 1500

$log = Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) {
    $text = Read-SharedText $log.FullName
    if ($text) {
        Write-Host ''
        Write-Host ("--- {0} ---" -f $log.Name)
        @($text -split "`n" | Where-Object { $_ -match 'console driver|renderer state|module (load|unload)|re-arming' }) |
            Select-Object -Last 14 | ForEach-Object { Write-Host ("  {0}" -f $_.TrimEnd()) }

        $worldDraw = 0
        foreach ($line in @($text -split "`n" | Where-Object { $_ -match 'objectDrawA=' })) {
            if ($line -match 'objectDrawA=(\d+)') { $worldDraw = [Math]::Max($worldDraw, [int]$Matches[1]) }
        }
        $camera = @($text -split "`n" | Where-Object { $_ -match '_ZCAMERA_MouseConCam #' }).Count
        Write-Host ''
        Write-Host ("  max objectDrawA  : {0}" -f $worldDraw)
        Write-Host ("  camera hook hits : {0}" -f $camera)
        if ($worldDraw -gt 0) { Write-Host '  VERDICT: a level is rendering' }
        else { Write-Host '  VERDICT: still no world geometry' }
    }
}

if (-not $KeepOpen) {
    Get-GameProcess | ForEach-Object { $_.Kill() }
    Write-Host 'game closed.'
}
