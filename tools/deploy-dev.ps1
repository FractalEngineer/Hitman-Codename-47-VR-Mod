<#
.SYNOPSIS
    Copy freshly built artifacts into the development game copy.

.DESCRIPTION
    Drops the app-local opengl32 proxy and the diagnostic launcher into
    .gamedev/Hitman. The Steam installation is never touched.

    h47vr_mod.dll is copied alongside for the injection path and for
    inspection; the proxy does not need it.

.PARAMETER BuildDir
    Build directory. Defaults to build/ in this repository.

.PARAMETER Dest
    Development game copy. Defaults to .gamedev/Hitman.

.EXAMPLE
    ./tools/build.ps1 ; ./tools/deploy-dev.ps1
#>
[CmdletBinding()]
param(
    [string]$BuildDir,
    [string]$Dest
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }
if (-not $Dest) { $Dest = Join-Path $repoRoot '.gamedev/Hitman' }

$binDir = Join-Path $BuildDir 'bin'
if (-not (Test-Path -LiteralPath $binDir)) { throw "No build output in $binDir. Run tools/build.ps1 first." }
if (-not (Test-Path -LiteralPath (Join-Path $Dest 'Hitman.Exe'))) {
    throw "No development copy at $Dest. Run tools/make-dev-copy.ps1 first."
}

$artifacts = @('opengl32.dll', 'h47vr_mod.dll', 'h47vr-launcher.exe')
foreach ($name in $artifacts) {
    $source = Join-Path $binDir $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing artifact: $source" }
    Copy-Item -LiteralPath $source -Destination (Join-Path $Dest $name) -Force
    Write-Host ("deployed {0,-22} -> {1}" -f $name, $Dest)
}

Write-Host ''
Write-Host 'Deployed. The proxy loads automatically when the game starts from this copy.'
