<#
.SYNOPSIS
    Fetch the OpenXR SDK and build the 32-bit loader the mod ships with.

.DESCRIPTION
    No openxr_loader.dll exists on this machine in any architecture, and the
    loader is what an application links against to find the active runtime. This
    clones the pinned SDK tag, builds the loader for x86 with the Visual Studio
    toolchain, and installs it plus the headers under third_party/openxr.

    The SDK source tree is git-ignored; the built loader and headers are what the
    mod ships with.

    Loader is Apache-2.0 (Khronos Group). The tag is pinned so the dependency is
    reproducible.

.PARAMETER Tag
    SDK tag to build. Changing this is a deliberate dependency bump.

.PARAMETER Force
    Re-clone and rebuild even when the output already exists.

.EXAMPLE
    ./tools/fetch-openxr.ps1
#>
[CmdletBinding()]
param(
    [string]$Tag = 'release-1.1.65',
    [string]$SdkDir,
    [string]$OutputDir,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $SdkDir) { $SdkDir = Join-Path $repoRoot 'third_party/openxr-sdk' }
if (-not $OutputDir) { $OutputDir = Join-Path $repoRoot 'third_party/openxr' }

$loaderOut = Join-Path $OutputDir 'lib/x86/openxr_loader.dll'
if ((Test-Path -LiteralPath $loaderOut) -and -not $Force) {
    Write-Host "Already built: $loaderOut"
    Write-Host 'Pass -Force to rebuild.'
    exit 0
}

# --- source -----------------------------------------------------------------
if ($Force -and (Test-Path -LiteralPath $SdkDir)) {
    Remove-Item -LiteralPath $SdkDir -Recurse -Force
}
if (-not (Test-Path -LiteralPath (Join-Path $SdkDir 'CMakeLists.txt'))) {
    Write-Host "Cloning OpenXR-SDK-Source $Tag ..."
    git clone --depth 1 --branch $Tag https://github.com/KhronosGroup/OpenXR-SDK-Source.git $SdkDir
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

$cmake = (Get-Command cmake -ErrorAction Stop).Source
$buildDir = Join-Path $env:TEMP 'h47vr-openxr-build'
if (Test-Path -LiteralPath $buildDir) { Remove-Item -LiteralPath $buildDir -Recurse -Force }

Write-Host 'Configuring the 32-bit loader ...'
& $cmake -S $SdkDir -B $buildDir -G Ninja `
    '-DCMAKE_BUILD_TYPE=Release' `
    '-DBUILD_API_LAYERS=OFF' `
    '-DBUILD_TESTS=OFF' `
    '-DBUILD_CONFORMANCE_TESTS=OFF' `
    '-DDYNAMIC_LOADER=ON' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }

Write-Host 'Building openxr_loader ...'
& $cmake --build $buildDir --target openxr_loader
if ($LASTEXITCODE -ne 0) { throw "Loader build failed ($LASTEXITCODE)" }

$built = Get-ChildItem -LiteralPath $buildDir -Recurse -Filter 'openxr_loader.dll' |
    Select-Object -First 1
if (-not $built) { throw "openxr_loader.dll was not produced under $buildDir" }

# --- install ----------------------------------------------------------------
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $loaderOut) | Out-Null
Copy-Item -LiteralPath $built.FullName -Destination $loaderOut -Force

# Import library, so the mod can link against the loader.
$importLib = Join-Path $built.DirectoryName 'openxr_loader.lib'
if (Test-Path -LiteralPath $importLib) {
    Copy-Item -LiteralPath $importLib -Destination (Join-Path (Split-Path -Parent $loaderOut) 'openxr_loader.lib') -Force
} else {
    Write-Warning "No import library next to $($built.FullName); linking will need one."
}

$includeOut = Join-Path $OutputDir 'include/openxr'
New-Item -ItemType Directory -Force -Path $includeOut | Out-Null

# The public headers are generated from the specification during the build, so
# the build tree is authoritative. The source tree only carries
# openxr_platform_defines.h.
$generatedInclude = Join-Path $buildDir 'include/openxr'
if (Test-Path -LiteralPath $generatedInclude) {
    Copy-Item -Path (Join-Path $generatedInclude '*.h') -Destination $includeOut -Force
} else {
    Copy-Item -Path (Join-Path $SdkDir 'include/openxr/*.h') -Destination $includeOut -Force
}

foreach ($license in @('LICENSE', 'COPYING.adoc')) {
    $path = Join-Path $SdkDir $license
    if (Test-Path -LiteralPath $path) {
        Copy-Item -LiteralPath $path -Destination (Join-Path $OutputDir $license) -Force
    }
}

"OpenXR-SDK-Source $Tag" | Set-Content -LiteralPath (Join-Path $OutputDir 'SOURCE.txt') -Encoding UTF8

$size = (Get-Item -LiteralPath $loaderOut).Length
Write-Host ''
Write-Host ("loader : {0} ({1:N0} bytes)" -f $loaderOut, $size)
Write-Host ("headers: {0}" -f $includeOut)
