<#
.SYNOPSIS
    One-command development iteration: build, install, launch via Steam, show the log.

.DESCRIPTION
    The game's legacy steam.dll only accepts a launch performed by the Steam
    client, so this drives the whole loop the way it has to work:

        build -> install into the game directory -> steam -applaunch -> tail log

    Must run outside any restricted context: a sandboxed child cannot reach
    Steam's IPC and the game reports "Failed to find Steam." and stops.

.PARAMETER SkipBuild
    Use whatever is already in build/bin instead of rebuilding.

.PARAMETER CloseGame
    Terminate the game at the end instead of leaving it running.

.PARAMETER TimeoutSeconds
    How long to wait for the game window before giving up.

.PARAMETER UninstallAfter
    Remove the mod from the game directory when finished.

.EXAMPLE
    ./tools/iterate.ps1
    ./tools/iterate.ps1 -SkipBuild -CloseGame
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 90,
    [switch]$SkipBuild,
    [switch]$CloseGame,
    [switch]$UninstallAfter
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

function Read-SharedText([string]$Path) {
    try {
        $stream = [System.IO.File]::Open($Path,
            [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
        try {
            $reader = New-Object System.IO.StreamReader($stream)
            try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $stream.Dispose() }
    } catch {
        Write-Warning "Could not read $Path : $($_.Exception.Message)"
        return $null
    }
}

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

if (-not $SkipBuild) {
    Write-Host '--- build ---'
    & (Join-Path $PSScriptRoot 'build.ps1') -Configuration $Configuration
}

$GameDir = Resolve-GameDir -Requested $GameDir

$running = Get-Process | Where-Object { $_.ProcessName -eq 'Hitman' }
if ($running) {
    throw 'The game is already running. Close it first so the new module is loaded.'
}

Write-Host ''
Write-Host '--- install ---'
$manifestPath = Join-Path $GameDir 'h47vr-install.json'
if (Test-Path -LiteralPath $manifestPath) {
    # Already installed: refresh just the binaries so a full uninstall/install
    # round trip is not needed on every iteration.
    foreach ($name in @('opengl32.dll', 'h47vr_mod.dll')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "build/bin/$name") `
                  -Destination (Join-Path $GameDir $name) -Force
        Write-Host "  refreshed $name"
    }
} else {
    & (Join-Path $PSScriptRoot 'install-mod.ps1') -GameDir $GameDir
}

$logDir = Join-Path $GameDir 'h47vr-logs'
if (-not (Test-Path -LiteralPath $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
$preexisting = @(Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName)

Write-Host ''
Write-Host '--- launch via Steam ---'
Start-Process -FilePath 'C:\Program Files (x86)\Steam\steam.exe' -ArgumentList '-applaunch', '6900'

$deadline = (Get-Date).AddSeconds($TimeoutSeconds)
$game = $null
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 500
    $candidate = Get-Process -Name 'Hitman' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($candidate -and $candidate.MainWindowTitle -and $candidate.MainWindowTitle -ne 'Application error') {
        $game = $candidate
        break
    }
}

if (-not $game) {
    $stuck = Get-Process -Name 'Hitman' -ErrorAction SilentlyContinue
    if ($stuck) {
        Write-Warning "Game is up but reports an error window: '$($stuck.MainWindowTitle)'"
    } else {
        Write-Warning "No game window appeared within $TimeoutSeconds s."
    }
    exit 1
}

Write-Host "  pid $($game.Id), window '$($game.MainWindowTitle)'"

# Give the engine time to reach the renderer and flush the proxy's own lines.
Start-Sleep -Seconds 8

$game = Get-Process -Id $game.Id -ErrorAction SilentlyContinue
if ($game) {
    Write-Host ''
    Write-Host '--- process ---'
    try {
        $modules = @($game.Modules)
        $relevant = @($modules |
            Where-Object { $_.ModuleName -match 'opengl|RenderD3D|RenderOpenGL|h47vr' } |
            Select-Object -ExpandProperty ModuleName)
        if ($relevant.Count -gt 0) {
            $relevant | ForEach-Object { Write-Host "  $_" }
        } else {
            # Enumerating modules of a WOW64 process from a 64-bit host returns a
            # truncated list, so absence here proves nothing. The log is the
            # authoritative signal for whether the proxy loaded.
            Write-Host ("  window '{0}'; module enumeration returned only {1} entries and is" -f $game.MainWindowTitle, $modules.Count)
            Write-Host '  unreliable cross-bitness - trust the log below'
        }
    } catch {
        Write-Host "  (module query failed: $($_.Exception.Message))"
    }
}

$newLog = Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
    Where-Object { $preexisting -notcontains $_.FullName } |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1

Write-Host ''
if ($newLog) {
    Write-Host "--- $($newLog.Name) ---"
    $text = Read-SharedText $newLog.FullName
    if ($text) { Write-Host $text }
} else {
    Write-Host '--- no new log file: the proxy never reached mod startup ---'
}

if ($CloseGame) {
    Get-Process -Name 'Hitman' -ErrorAction SilentlyContinue | Stop-Process -Force
    Write-Host 'game terminated'
}

if ($UninstallAfter) {
    Write-Host ''
    & (Join-Path $PSScriptRoot 'uninstall-mod.ps1') -GameDir $GameDir
}
