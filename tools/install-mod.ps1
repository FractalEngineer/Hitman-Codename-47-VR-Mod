<#
.SYNOPSIS
    Install the mod into a game directory, reversibly.

.DESCRIPTION
    The Steam DRM in Hitman.Exe only accepts a launch performed by Steam, so the
    mod has to sit in the directory Steam launches from. This script installs the
    app-local opengl32 proxy, selects the OpenGL renderer and writes a manifest
    that tools/uninstall-mod.ps1 uses to put everything back.

    Nothing is overwritten without a backup: a pre-existing opengl32.dll aborts
    the install rather than being replaced.

.PARAMETER GameDir
    Target installation. Defaults to the Steam install.

.PARAMETER BuildDir
    Build output directory. Defaults to build/ in this repository.

.PARAMETER SkipRenderer
    Leave Hitman.ini alone.

.EXAMPLE
    ./tools/install-mod.ps1
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$BuildDir,
    [switch]$SkipRenderer
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }

if (-not $GameDir) {
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
            if (Test-Path -LiteralPath (Join-Path $candidate 'Hitman.Exe')) { $GameDir = $candidate; break }
        }
        if ($GameDir) { break }
    }
}
if (-not $GameDir) { throw 'Could not locate the game. Pass -GameDir explicitly.' }
$GameDir = (Resolve-Path -LiteralPath $GameDir).Path

$binDir = Join-Path $BuildDir 'bin'
foreach ($artifact in @('opengl32.dll', 'h47vr_mod.dll', 'openxr_loader.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $binDir $artifact))) {
        throw "Missing $artifact in $binDir. Run tools/build.ps1 first."
    }
}

$manifestPath = Join-Path $GameDir 'h47vr-install.json'
if (Test-Path -LiteralPath $manifestPath) { throw "Already installed. Run tools/uninstall-mod.ps1 first." }

$proxyTarget = Join-Path $GameDir 'opengl32.dll'
if (Test-Path -LiteralPath $proxyTarget) {
    throw "Refusing to overwrite an existing opengl32.dll in the game directory: $proxyTarget"
}

$existing = Get-Process | Where-Object { $_.ProcessName -eq 'Hitman' }
if ($existing) { throw 'The game is running. Close it before installing.' }

Write-Host "Installing into $GameDir"

$added = [System.Collections.Generic.List[string]]::new()
foreach ($artifact in @('opengl32.dll', 'h47vr_mod.dll', 'openxr_loader.dll')) {
    Copy-Item -LiteralPath (Join-Path $binDir $artifact) -Destination (Join-Path $GameDir $artifact) -Force
    $added.Add($artifact)
    Write-Host "  added $artifact"
}

# --- renderer selection -----------------------------------------------------
$iniPath = Join-Path $GameDir 'Hitman.ini'
$iniBackup = Join-Path $GameDir 'Hitman.ini.h47vr-backup'
$previousDrawDll = $null

if (-not $SkipRenderer) {
    $iniBytes = [System.IO.File]::ReadAllBytes($iniPath)
    $originalHash = (Get-FileHash -LiteralPath $iniPath -Algorithm SHA256).Hash

    if (-not (Test-Path -LiteralPath $iniBackup)) {
        Copy-Item -LiteralPath $iniPath -Destination $iniBackup
    }

    $lines = Get-Content -LiteralPath $iniPath
    $previousDrawDll = ($lines | Where-Object { $_ -match '^\s*DrawDll\b' } | Select-Object -First 1)

    $replaced = $false
    $out = foreach ($line in $lines) {
        if ($line -match '^\s*DrawDll\b') {
            if (-not $replaced) { $replaced = $true; 'DrawDll RenderOpenGL.dll'; continue }
            continue
        }
        $line
    }
    if (-not $replaced) { $out = @('DrawDll RenderOpenGL.dll') + $out }
    Set-Content -LiteralPath $iniPath -Value $out -Encoding ASCII

    Write-Host "  renderer set to RenderOpenGL.dll (was: $previousDrawDll)"
}

$manifest = [pscustomobject]@{
    installedUtc      = (Get-Date).ToUniversalTime().ToString('o')
    repoRoot          = $repoRoot
    gameDir           = $GameDir
    addedFiles        = $added
    rendererChanged   = (-not $SkipRenderer)
    iniPath           = $iniPath
    iniBackup         = $(if ($SkipRenderer) { $null } else { $iniBackup })
    iniOriginalHash   = $(if ($SkipRenderer) { $null } else { $originalHash })
    previousDrawDll   = $previousDrawDll
}
$manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $manifestPath -Encoding UTF8

Write-Host ''
Write-Host 'Installed. Launch the game from Steam; the proxy loads automatically.'
Write-Host "Undo with: ./tools/uninstall-mod.ps1 -GameDir `"$GameDir`""
