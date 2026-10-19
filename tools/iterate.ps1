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

.PARAMETER Scene
    One or more scenes to try, in order, one per attempt. Written to
    <game>\h47vr-scene.txt as separate lines, which the mod's console driver
    reads at startup and submits to the engine's own command dispatcher - no
    keystrokes and no intro skip needed. Omit to run with the driver idle.
    Scene names are relative paths, e.g. 'C0_Training\C0_1'.

.PARAMETER SettleSeconds
    How long to watch the log for a verdict (world geometry appearing, or the
    game dying) before dumping it. Stops early as soon as either happens.

.EXAMPLE
    ./tools/iterate.ps1
    ./tools/iterate.ps1 -SkipBuild -CloseGame
    ./tools/iterate.ps1 -Scene C0_1
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [int]$TimeoutSeconds = 90,
    [int]$SettleSeconds = 45,
    [string[]]$Scene,
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
    foreach ($name in @('opengl32.dll', 'h47vr_mod.dll', 'openxr_loader.dll')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "build/bin/$name") `
                  -Destination (Join-Path $GameDir $name) -Force
        Write-Host "  refreshed $name"
    }
} else {
    & (Join-Path $PSScriptRoot 'install-mod.ps1') -GameDir $GameDir
}

# The console driver reads this at startup. A stale request would silently
# change the next run, so the file is always written explicitly.
$sceneFile = Join-Path $GameDir 'h47vr-scene.txt'
if ($Scene) {
    Set-Content -LiteralPath $sceneFile -Value $Scene -Encoding ascii
    Write-Host ("  scene requests: {0}" -f ($Scene -join ', '))
} elseif (Test-Path -LiteralPath $sceneFile) {
    Remove-Item -LiteralPath $sceneFile -Force
    Write-Host '  removed a stale scene request'
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

# Watch the log until there is a verdict rather than guessing how long the
# engine needs. The mod submits its scene request a few seconds in, so the
# interesting lines arrive without any input from the user.
$settleDeadline = (Get-Date).AddSeconds($SettleSeconds)
$verdict = "still watching after $SettleSeconds s"
while ((Get-Date) -lt $settleDeadline) {
    Start-Sleep -Milliseconds 500
    if (-not (Get-Process -Id $game.Id -ErrorAction SilentlyContinue)) {
        $verdict = 'the game exited'
        break
    }
    $candidate = Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
        Where-Object { $preexisting -notcontains $_.FullName } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $candidate) { continue }
    $snapshot = Read-SharedText $candidate.FullName
    if ($snapshot -and $snapshot -match 'objectDrawA=[1-9]') {
        $verdict = 'world geometry detected'
        break
    }
}
Write-Host ("  watch result: {0}" -f $verdict)

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
    if ($text) {
        Write-Host $text

        $lines = @($text -split "`n")
        $counters = @($lines | Where-Object { $_ -match 'renderer calls' })
        $worldDraw = 0
        foreach ($line in $counters) {
            if ($line -match 'objectDrawA=(\d+)') {
                $worldDraw = [Math]::Max($worldDraw, [int]$Matches[1])
            }
        }
        $camera = @($lines | Where-Object { $_ -match '_ZCAMERA_MouseConCam #' }).Count
        $console = @($lines | Where-Object { $_ -match 'console line #' }).Count
        $attempts = @($lines | Where-Object { $_ -match "console driver: 'startscene" })

        Write-Host ''
        Write-Host '--- summary ---'
        Write-Host ("  renderer reports   : {0}" -f $counters.Count)
        Write-Host ("  max objectDrawA    : {0}" -f $worldDraw)
        Write-Host ("  camera hook hits   : {0}" -f $camera)
        Write-Host ("  console lines seen : {0}" -f $console)
        foreach ($attempt in $attempts) { Write-Host ("  {0}" -f $attempt.Trim()) }
        if ($worldDraw -gt 0) {
            Write-Host '  VERDICT: a level is rendering'
        } else {
            Write-Host '  VERDICT: still no world geometry'
        }
    }
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
