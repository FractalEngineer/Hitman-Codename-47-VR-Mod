<#
.SYNOPSIS
    Create an isolated development copy of the game and optionally select the
    OpenGL renderer in that copy only.

.DESCRIPTION
    The Steam installation is the reference and is never modified by this mod.
    This script mirrors it into .gamedev/Hitman (git-ignored) where the proxy
    DLL and the built mod are dropped during development.

    Existing files in the destination are overwritten only when the source is
    newer, so the copy can be refreshed without deleting your build artifacts.

.PARAMETER GameDir
    Source game directory. Defaults to the Steam install.

.PARAMETER Dest
    Destination directory. Defaults to .gamedev/Hitman in this repository.

.PARAMETER SwitchToOpenGL
    Rewrite DrawDll in the copy's Hitman.ini to select RenderOpenGL.dll. The
    original ini is preserved once as Hitman.ini.steam-original.

.PARAMETER Force
    Re-copy every file even when timestamps match.

.EXAMPLE
    ./tools/make-dev-copy.ps1 -SwitchToOpenGL
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$Dest,
    [switch]$SwitchToOpenGL,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $Dest) { $Dest = Join-Path $repoRoot '.gamedev/Hitman' }

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

# Refuse to write a dev copy on top of a Steam installation.
if ($GameDir -eq $Dest) { throw 'Source and destination are the same directory.' }
if ($Dest -match 'steamapps') { throw "Refusing to write a dev copy inside a Steam library: $Dest" }

Write-Host "Source      : $GameDir"
Write-Host "Destination : $Dest"

$roboArgs = @(
    $GameDir, $Dest,
    '/E',                # include subdirectories, including empty ones
    '/COPY:DAT',         # data, attributes, timestamps (never ACLs on a vendored copy)
    '/R:2', '/W:1',      # retry twice, one second apart
    '/NFL', '/NDL',      # no per-file or per-directory listing
    '/NJH', '/NJS',      # quiet header and summary
    '/NP'                # no progress
)
if ($Force) { $roboArgs += '/IS' }

& robocopy @roboArgs | Out-Null
$rc = $LASTEXITCODE
if ($rc -ge 8) { throw "robocopy failed with exit code $rc" }
Write-Host "Copied (robocopy exit $rc)."

if ($SwitchToOpenGL) {
    $iniPath = Join-Path $Dest 'Hitman.ini'
    if (-not (Test-Path -LiteralPath $iniPath)) { throw "Missing Hitman.ini in $Dest" }

    $backup = "$iniPath.steam-original"
    if (-not (Test-Path -LiteralPath $backup)) {
        Copy-Item -LiteralPath $iniPath -Destination $backup
        Write-Host "Preserved original config as $(Split-Path -Leaf $backup)."
    }

    $lines = Get-Content -LiteralPath $iniPath
    $found = $false
    $out = foreach ($line in $lines) {
        if ($line -match '^\s*(//)?\s*DrawDll\b') {
            if (-not $found -and $line -notmatch '^\s*//') {
                $found = $true
                'DrawDll RenderOpenGL.dll'
                continue
            }
            if ($line -match '^\s*//\s*DrawDll\s+RenderOpenGL') { continue }  # drop stale comment
        }
        $line
    }
    if (-not $found) { $out = @('DrawDll RenderOpenGL.dll') + $out }

    Set-Content -LiteralPath $iniPath -Value $out -Encoding ASCII
    Write-Host 'Renderer set to RenderOpenGL.dll in the development copy.'
}

Write-Host 'Done.'
