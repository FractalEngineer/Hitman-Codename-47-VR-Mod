<#
.SYNOPSIS
    Remove the mod and restore the commercial copy.

.DESCRIPTION
    Uses the manifest written by tools/install-mod.ps1 to delete exactly the
    files that were added and to restore Hitman.ini from its backup. Finishes by
    re-verifying the build pins, so a successful run proves the installation is
    byte-for-byte back to the supported build.

.EXAMPLE
    ./tools/uninstall-mod.ps1
#>
[CmdletBinding()]
param(
    [string]$GameDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $GameDir) {
    # Only a directory that actually carries a manifest can be uninstalled, so
    # search for the manifest rather than reusing the fingerprint default.
    $candidates = [System.Collections.Generic.List[string]]::new()
    $candidates.Add((Join-Path $repoRoot '.gamedev/Hitman'))

    $vdfs = @(
        'C:\Program Files (x86)\Steam\steamapps\libraryfolders.vdf',
        'C:\Program Files\Steam\steamapps\libraryfolders.vdf'
    )
    foreach ($vdf in $vdfs) {
        if (-not (Test-Path -LiteralPath $vdf)) { continue }
        $roots = Select-String -LiteralPath $vdf -Pattern '"path"\s+"(.+?)"' -AllMatches |
            ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value -replace '\\\\', '\' }
        foreach ($root in $roots) {
            $candidates.Add((Join-Path $root 'steamapps/common/Hitman Codename 47'))
        }
    }

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath (Join-Path $candidate 'h47vr-install.json')) {
            $GameDir = $candidate
            break
        }
    }
}
if (-not $GameDir) { throw 'Could not locate the game. Pass -GameDir explicitly.' }
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path

$manifestPath = Join-Path $GameDir 'h47vr-install.json'
if (-not (Test-Path -LiteralPath $manifestPath)) {
    throw "No install manifest in $GameDir; nothing to uninstall."
}
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json

$running = Get-Process | Where-Object { $_.ProcessName -eq 'Hitman' }
if ($running) { throw 'The game is running. Close it before uninstalling.' }

Write-Host "Removing the mod from $($manifest.gameDir)"

foreach ($name in $manifest.addedFiles) {
    $path = Join-Path $manifest.gameDir $name
    if (Test-Path -LiteralPath $path) {
        Remove-Item -LiteralPath $path -Force
        Write-Host "  removed $name"
    }
}

if ($manifest.rendererChanged -and $manifest.iniBackup) {
    if (Test-Path -LiteralPath $manifest.iniBackup) {
        Copy-Item -LiteralPath $manifest.iniBackup -Destination $manifest.iniPath -Force
        Remove-Item -LiteralPath $manifest.iniBackup -Force
        Write-Host '  restored Hitman.ini'
    } else {
        Write-Host "  WARNING: backup $($manifest.iniBackup) is missing; Hitman.ini left as-is"
    }
}

Remove-Item -LiteralPath $manifestPath -Force

# The proxy writes its diagnostics next to the game.
$logDir = Join-Path $manifest.gameDir 'h47vr-logs'
if (Test-Path -LiteralPath $logDir) {
    Remove-Item -LiteralPath $logDir -Recurse -Force
    Write-Host '  removed h47vr-logs'
}

Write-Host ''
Write-Host 'Verifying the restored installation:'
& (Join-Path $PSScriptRoot 'fingerprint.ps1') -GameDir $GameDir
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'Build pins do not match after uninstall. Inspect the game directory.'
    exit $LASTEXITCODE
}
Write-Host 'Uninstalled cleanly; the supported build is intact.'
