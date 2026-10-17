<#
.SYNOPSIS
    Configure and build the 32-bit mod with the Visual Studio x86 toolchain.

.PARAMETER Configuration
    Release (default) or Debug.

.PARAMETER Clean
    Delete the build directory first.

.PARAMETER Target
    Optional CMake target to build instead of everything.

.EXAMPLE
    ./tools/build.ps1
    ./tools/build.ps1 -Configuration Debug -Target h47vr_launcher
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Configuration = 'Release',
    [string]$BuildDir,
    [string]$Target,
    [switch]$Clean,
    [switch]$RegenerateProxy
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $repoRoot 'build' }

# --- locate the x86 toolchain -----------------------------------------------
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere.exe not found; install the VS C++ workload.' }

$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'No Visual Studio install with the x86 C++ tools was found.' }
$vsPath = $vsPath.Trim()

$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvarsall.bat'
if (-not (Test-Path -LiteralPath $vcvars)) { throw "vcvarsall.bat not found under $vsPath" }

# Import the x86 developer environment into this session.
$environment = & cmd.exe /c "`"$vcvars`" x86 >nul 2>&1 && set"
foreach ($line in $environment) {
    $split = $line.IndexOf('=')
    if ($split -lt 1) { continue }
    $name = $line.Substring(0, $split)
    $value = $line.Substring($split + 1)
    Set-Item -Path "Env:$name" -Value $value
}
Write-Host "Toolchain: $vsPath (x86)"

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { throw 'cmake was not found on PATH.' }

if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

if ($RegenerateProxy) {
    $python = (Get-Command python -ErrorAction SilentlyContinue).Source
    if (-not $python) { throw 'python is required for -RegenerateProxy.' }

    # The proxy must shadow the whole surface of the system library: gdi32's
    # pixel format functions call opengl32!wgl* at run time, so an
    # import-scoped proxy breaks the game.
    $systemOpenGL = Join-Path $env:WINDIR 'SysWOW64\opengl32.dll'
    if (-not (Test-Path -LiteralPath $systemOpenGL)) {
        throw "Cannot regenerate the proxy: $systemOpenGL not found."
    }
    & $python (Join-Path $PSScriptRoot 'gen_opengl32_proxy.py') `
        --exports-of $systemOpenGL `
        --cpp (Join-Path $repoRoot 'src/proxy_opengl32/opengl32_exports.generated.cpp') `
        --def (Join-Path $repoRoot 'src/proxy_opengl32/opengl32.def') `
        --slots-name h47vr_proxy_slots --names-name h47vr_proxy_names --count-name h47vr_proxy_count
}

$configureArgs = @(
    '-S', $repoRoot,
    '-B', $BuildDir,
    '-G', 'Ninja',
    "-DCMAKE_BUILD_TYPE=$Configuration"
)
& $cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

$buildArgs = @('--build', $BuildDir, '--config', $Configuration)
if ($Target) { $buildArgs += @('--target', $Target) }
& $cmake @buildArgs
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

Write-Host ''
Write-Host "Artifacts in $(Join-Path $BuildDir 'bin'):"
Get-ChildItem (Join-Path $BuildDir 'bin') -ErrorAction SilentlyContinue |
    Select-Object Length, Name | Format-Table -AutoSize | Out-String | Write-Host
