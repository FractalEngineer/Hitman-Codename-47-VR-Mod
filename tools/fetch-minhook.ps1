<#
.SYNOPSIS
    Fetch MinHook and build the 32-bit static library the mod links against.

.DESCRIPTION
    MinHook is compiled directly from its sources rather than through its own
    build files, so the dependency is just four C files and one header, and the
    result matches the rest of the build (MSVC x86, /MD).

    The source tree is git-ignored; the header, licence and built library are
    committed under third_party/minhook.

.PARAMETER Tag
    MinHook tag to build. Changing this is a deliberate dependency bump.

.PARAMETER Force
    Re-clone and rebuild even when the output already exists.

.EXAMPLE
    ./tools/fetch-minhook.ps1
#>
[CmdletBinding()]
param(
    [string]$Tag = 'v1.3.4',
    [string]$SourceDir,
    [string]$OutputDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SourceDir) { $SourceDir = Join-Path $repoRoot 'third_party/minhook-src' }
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'third_party/minhook' }

$libOut = Join-Path $OutputDir 'lib/x86/minhook.lib'
if ((Test-Path -LiteralPath $libOut) -and -not $Force) {
    Write-Host "Already built: $libOut"
    Write-Host 'Pass -Force to rebuild.'
    exit 0
}

if ($Force -and (Test-Path -LiteralPath $SourceDir)) {
    Remove-Item -LiteralPath $SourceDir -Recurse -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $SourceDir 'include/MinHook.h'))) {
    Write-Host "Cloning MinHook $Tag ..."
    git clone --depth 1 --branch $Tag https://github.com/TsudaKageyu/minhook.git $SourceDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed ($LASTEXITCODE)" }
}

# --- 32-bit toolchain -------------------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found; install the VS C++ workload.' }
$vsPath = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath).Trim()
if (-not $vsPath) { throw 'No Visual Studio install with the x86 C++ tools was found.' }

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
$environment = & cmd.exe /c "`"$vcvars`" x86 >nul 2>&1 && set"
foreach ($line in $environment) {
    $split = $line.IndexOf('=')
    if ($split -lt 1) { continue }
    Set-Item -Path "Env:$($line.Substring(0, $split))" -Value $line.Substring($split + 1)
}

$work = Join-Path $env:TEMP 'h47vr-minhook-build'
if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
New-Item -ItemType Directory -Force -Path $work | Out-Null

$sources = @(
    'src/hook.c',
    'src/trampoline.c',
    'src/buffer.c',
    'src/hde/hde32.c'
)

Push-Location $work
try {
    $cl = (Get-Command cl.exe -ErrorAction Stop).Source
    $compileArgs = @(
        '/nologo', '/c', '/O2', '/W3', '/DNDEBUG', '/DWIN32', '/D_WINDOWS', '/MD',
        "/I$SourceDir/include", "/I$SourceDir/src", "/I$SourceDir/src/hde"
    ) + ($sources | ForEach-Object { Join-Path $SourceDir $_ })

    Write-Host 'Compiling MinHook for x86 ...'
    & $cl @compileArgs
    if ($LASTEXITCODE -ne 0) { throw "cl failed ($LASTEXITCODE)" }

    & lib.exe /nologo /OUT:minhook.lib hook.obj trampoline.obj buffer.obj hde32.obj
    if ($LASTEXITCODE -ne 0) { throw "lib failed ($LASTEXITCODE)" }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $libOut) | Out-Null
    New-Item -ItemType Directory -Force -Path (Join-Path $OutputDir 'include') | Out-Null
    Copy-Item -LiteralPath (Join-Path $work 'minhook.lib') -Destination $libOut -Force
    Copy-Item -LiteralPath (Join-Path $SourceDir 'include/MinHook.h') -Destination (Join-Path $OutputDir 'include') -Force
    Copy-Item -LiteralPath (Join-Path $SourceDir 'LICENSE.txt') -Destination (Join-Path $OutputDir 'LICENSE.txt') -Force
    "MinHook $Tag" | Set-Content -LiteralPath (Join-Path $OutputDir 'SOURCE.txt') -Encoding UTF8
} finally {
    Pop-Location
}

Write-Host ''
Write-Host ("library : {0} ({1:N0} bytes)" -f $libOut, (Get-Item -LiteralPath $libOut).Length)
