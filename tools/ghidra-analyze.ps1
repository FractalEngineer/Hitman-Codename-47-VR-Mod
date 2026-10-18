<#
.SYNOPSIS
    Run Ghidra headless over the game binaries and extract reverse-engineering anchors.

.DESCRIPTION
    Imports the modules the mod has to reason about into a throwaway Ghidra
    project, runs auto-analysis, then runs tools/ghidra/ExtractAnchors.java once
    per program. Each program produces a report of string anchors, their
    cross-references and the containing functions, plus every MSVC RTTI type
    descriptor found.

    Reports land in tools/_ghidra_out (git-ignored) with one .anchors.txt file
    per program. Nothing in the game directory is modified.

.PARAMETER GameDir
    Game directory to analyse. Defaults to the Steam install.

.PARAMETER GhidraHome
    Ghidra installation. Defaults to the standard tools location.

.PARAMETER JavaHome
    JDK to run Ghidra with. Defaults to the standard tools location.

.PARAMETER Reanalyze
    Delete the existing Ghidra project first, forcing a full auto-analysis.

.EXAMPLE
    ./tools/ghidra-analyze.ps1
    ./tools/ghidra-analyze.ps1 -Reanalyze
#>
[CmdletBinding()]
param(
    [string]$GameDir,
    [string]$GhidraHome = 'C:\Users\titan\tools\ghidra_12.1.4_PUBLIC',
    [string]$JavaHome = 'C:\Users\titan\tools\jdk-21.0.12.1+1',
    [string[]]$Modules = @('RenderOpenGL.dll', 'System.dll', 'Globals.dll', 'EngineData.dll', 'HitmanDlc.dlc'),
    [switch]$Reanalyze
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

# Accept both "-Modules a,b" (one literal string when invoked via -File) and a
# real PowerShell array.
$Modules = @($Modules | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

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

$headless = Join-Path $GhidraHome 'support/analyzeHeadless.bat'
if (-not (Test-Path -LiteralPath $headless)) { throw "analyzeHeadless.bat not found under $GhidraHome" }
if (-not (Test-Path -LiteralPath (Join-Path $JavaHome 'bin/java.exe'))) { throw "java.exe not found under $JavaHome" }

$env:JAVA_HOME = $JavaHome

# Ghidra rejects project paths containing a path element that starts with '.',
# so this cannot be a dot-directory.
$projectDir = Join-Path $repoRoot 'ghidra_project'
$projectName = 'h47vr'
$outDir = Join-Path $repoRoot 'tools/_ghidra_out'
New-Item -ItemType Directory -Force -Path $projectDir, $outDir | Out-Null

if ($Reanalyze) {
    Get-ChildItem -LiteralPath $projectDir -Force | Remove-Item -Recurse -Force
    Write-Host 'cleared the existing Ghidra project'
}

$scriptDir = Join-Path $PSScriptRoot 'ghidra'

$importArgs = [System.Collections.Generic.List[string]]::new()
foreach ($module in $Modules) {
    $path = Join-Path $GameDir $module
    if (-not (Test-Path -LiteralPath $path)) { throw "Missing module: $path" }
    $importArgs.Add('-import')
    $importArgs.Add($path)
}

$args = @(
    $projectDir, $projectName
) + $importArgs + @(
    '-overwrite',
    '-scriptPath', $scriptDir,
    '-postScript', 'ExtractAnchors.java', $outDir,
    '-analysisTimeoutPerFile', '1800'
)

Write-Host "Ghidra  : $GhidraHome"
Write-Host "JDK     : $JavaHome"
Write-Host "Game    : $GameDir"
Write-Host "Modules : $($Modules -join ', ')"
Write-Host "Reports : $outDir"
Write-Host ''

& $headless @args
if ($LASTEXITCODE -ne 0) { throw "analyzeHeadless failed with exit code $LASTEXITCODE" }

Write-Host ''
Write-Host 'Anchor reports:'
Get-ChildItem -LiteralPath $outDir -Filter '*.anchors.txt' |
    Sort-Object Name | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
