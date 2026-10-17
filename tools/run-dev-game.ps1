<#
.SYNOPSIS
    Launch the development copy and report what actually happened.

.DESCRIPTION
    The game requires the Steam client, so this must run outside any restricted
    context: a sandboxed child cannot reach Steam's IPC and the game reports
    "Failed to find Steam." and stops.

    Reports the exit code, the module list while the process is alive, and the
    contents of h47vr-logs if the proxy produced one.

.PARAMETER GameDir
    Game directory to launch. Defaults to .gamedev/Hitman.

.PARAMETER ObserveSeconds
    How long to watch the process before deciding it is up and killing it.

.PARAMETER LeaveRunning
    Do not terminate the process afterwards.

.EXAMPLE
    ./tools/run-dev-game.ps1 -ObserveSeconds 25
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [int]$ObserveSeconds = 20,
    [switch]$LeaveRunning
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $GameDir) { $GameDir = Join-Path $repoRoot '.gamedev/Hitman' }
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path

$exe = Join-Path $GameDir 'Hitman.Exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Hitman.Exe not found in $GameDir" }

$logDir = Join-Path $GameDir 'h47vr-logs'
if (Test-Path -LiteralPath $logDir) { Remove-Item -LiteralPath $logDir -Recurse -Force }

Write-Host "launching $exe"
$started = Get-Date
$process = Start-Process -FilePath $exe -WorkingDirectory $GameDir -PassThru

$exited = $false
for ($i = 0; $i -lt $ObserveSeconds; $i++) {
    Start-Sleep -Seconds 1
    $live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
    if (-not $live) { $exited = $true; break }
}

$live = Get-Process -Id $process.Id -ErrorAction SilentlyContinue
if ($exited) {
    $process.Refresh()
    Write-Host ("process exited after ~{0}s with code 0x{1:X8} ({1})" -f $i, $process.ExitCode)
} else {
    Write-Host "process is alive after $ObserveSeconds s"
    $live | Select-Object Id, ProcessName, MainWindowTitle,
        @{n = 'WS(MB)'; e = { [int]($_.WorkingSet64 / 1MB) } } | Format-Table -AutoSize

    Write-Host 'relevant loaded modules:'
    try {
        $live.Modules |
            Where-Object { $_.ModuleName -match 'opengl|h47vr|RenderOpenGL|RenderD3D' } |
            ForEach-Object { Write-Host ("  {0,-20} {1}" -f $_.ModuleName, $_.FileName) }
    } catch {
        Write-Host "  (module query failed: $($_.Exception.Message))"
    }
}

Write-Host ''
if (Test-Path -LiteralPath $logDir) {
    Write-Host 'h47vr log:'
    Get-ChildItem -LiteralPath $logDir -Filter *.log |
        ForEach-Object { Get-Content -LiteralPath $_.FullName }
} else {
    Write-Host 'no h47vr log directory was created - the proxy never reached mod startup'
}

if ($live -and -not $LeaveRunning) {
    $live | Stop-Process -Force
    Write-Host ''
    Write-Host 'process terminated'
}
