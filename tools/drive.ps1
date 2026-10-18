<#
.SYNOPSIS
    Ask the running game to load a scene, without touching the keyboard.

.DESCRIPTION
    Writes <game>\h47vr-go.txt. The mod polls for that file on the engine's
    frame boundary, submits `startscene <scene>` to the engine's own command
    dispatcher, and deletes the file again.

    This is the reliable way to enter a level: the mod is already running, so
    the request lands at whatever moment it is issued. That matters because the
    scheduled, from-start-up path always fires while the intro videos are still
    playing - the intro has to be skipped by hand first.

    Scene names are relative paths without the extension, straight out of the
    game's own level-select table: 'C0_Training\C0_1', 'C1_HongKong\C1_1', ...

.PARAMETER Scene
    Scene to load, for example 'C0_Training\C0_1'.

.PARAMETER WatchSeconds
    How long to follow the newest log for the result, once the request is
    written.

.EXAMPLE
    ./tools/drive.ps1 -Scene 'C0_Training\C0_1'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Scene,
    [string]$GameDir,
    [int]$WatchSeconds = 15
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

function Read-SharedText([string]$Path) {
    try {
        $stream = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $reader = New-Object System.IO.StreamReader($stream)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
    } catch {
        return $null
    }
}

$GameDir = Resolve-GameDir -Requested $GameDir

$game = Get-Process -Name Hitman -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $game) { throw 'The game is not running. Start it first; the mod fires the request in-process.' }

$goFile = Join-Path $GameDir 'h47vr-go.txt'
Set-Content -LiteralPath $goFile -Value $Scene -Encoding ascii
Write-Host ("request written: {0} -> '{1}'" -f $goFile, $Scene)
Write-Host ("game pid {0}, window '{1}'" -f $game.Id, $game.MainWindowTitle)

$logDir = Join-Path $GameDir 'h47vr-logs'
$after = Get-Date

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
        Write-Host ("--- {0}, most recent lines ---" -f $log.Name)
        $lines = @($text -split "`n" | Where-Object { $_ -match 'console driver|renderer state|startscene|camera hook|_ZCAMERA|module ' })
        $lines | Select-Object -Last 12 | ForEach-Object { Write-Host ("  {0}" -f $_.TrimEnd()) }

        $worldDraw = 0
        foreach ($line in @($text -split "`n" | Where-Object { $_ -match 'objectDrawA=(\d+)' })) {
            if ($line -match 'objectDrawA=(\d+)') { $worldDraw = [Math]::Max($worldDraw, [int]$Matches[1]) }
        }
        $camera = @($text -split "`n" | Where-Object { $_ -match '_ZCAMERA_MouseConCam #' }).Count
        Write-Host ''
        Write-Host ("  max objectDrawA  : {0}" -f $worldDraw)
        Write-Host ("  camera hook hits : {0}" -f $camera)
    }
}
