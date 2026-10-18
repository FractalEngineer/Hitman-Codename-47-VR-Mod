<#
.SYNOPSIS
    Run SteamVR headless, using its null driver, as a 32-bit OpenXR runtime.

.DESCRIPTION
    SteamVR ships a genuine 32-bit OpenXR runtime (bin\vrclient.dll, i386) and a
    null HMD driver (drivers\null). Enabling the null driver lets the whole
    graphics-bridge gate be exercised with no headset attached.

    This edits steamvr.vrsettings, so the original is backed up first and
    -Action Disable puts it back. The script never touches the game directory.

    Note this corrects a plan assumption: the plan recorded SteamVR as 64-bit
    only. That was about the OpenXR *loader*, not the runtime. SteamVR's own
    32-bit OpenXR runtime is present and usable.

.PARAMETER Action
    Enable  - back up the config, enable the null driver, and start SteamVR.
    Disable - stop SteamVR and restore the backed-up config.
    Status  - report the current state without changing anything.

.PARAMETER SteamVrHome
    SteamVR install directory. Discovered from libraryfolders.vdf by default.

.PARAMETER NoStart
    With Enable, change the config but do not start SteamVR.

.EXAMPLE
    ./tools/steamvr-headless.ps1 -Action Status
    ./tools/steamvr-headless.ps1 -Action Enable
    ./tools/steamvr-headless.ps1 -Action Disable
#>
[CmdletBinding()]
param(
    [ValidateSet('Enable', 'Disable', 'Status')]
    [string]$Action = 'Status',
    [string]$SteamVrHome,
    [switch]$NoStart
)

$ErrorActionPreference = 'Stop'

function Resolve-SteamVrHome {
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
            $candidate = Join-Path $root 'steamapps/common/SteamVR'
            if (Test-Path -LiteralPath (Join-Path $candidate 'bin/vrclient.dll')) { return $candidate }
        }
    }
    throw 'SteamVR not found. Pass -SteamVrHome explicitly.'
}

$vr = Resolve-SteamVrHome -Requested $SteamVrHome
$steamRoot = (Resolve-Path -LiteralPath (Join-Path $vr '..\..\..')).Path
$configPath = Join-Path $steamRoot 'config/steamvr.vrsettings'
$backupPath = "$configPath.h47vr-backup"

$client32 = Join-Path $vr 'bin/vrclient.dll'
$nullDriver = Join-Path $vr 'drivers/null/bin/win32/driver_null.dll'

Write-Host "SteamVR     : $vr"
Write-Host "config      : $configPath"
Write-Host ("32-bit runtime present : {0}" -f (Test-Path -LiteralPath $client32))
Write-Host ("null driver present    : {0}" -f (Test-Path -LiteralPath $nullDriver))

$running = @(Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.ProcessName -match '^(vrserver|vrmonitor|vrcompositor|vrdashboard)$' })

$settings = if (Test-Path -LiteralPath $configPath) {
    Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
} else {
    New-Object psobject
}

$nullEnabled = $false
if ($settings.PSObject.Properties.Name -contains 'driver_null') {
    $nullEnabled = [bool]$settings.driver_null.enable
}

Write-Host ''
Write-Host ("SteamVR running        : {0} ({1} process(es))" -f ($running.Count -gt 0), $running.Count)
Write-Host ("null driver enabled    : {0}" -f $nullEnabled)
Write-Host ("config backup present  : {0}" -f (Test-Path -LiteralPath $backupPath))

switch ($Action) {
    'Status' {
        if (-not $nullEnabled) {
            Write-Host ''
            Write-Host 'Run with -Action Enable to switch on the null driver and start SteamVR.'
        }
        exit 0
    }

    'Enable' {
        if (-not (Test-Path -LiteralPath $client32)) { throw "Missing $client32" }
        if (-not (Test-Path -LiteralPath $nullDriver)) { throw "Missing $nullDriver" }

        if (-not (Test-Path -LiteralPath $backupPath)) {
            Copy-Item -LiteralPath $configPath -Destination $backupPath -Force
            Write-Host ''
            Write-Host "Backed up the original config to $backupPath"
        }

        # Enable the null HMD. loadPriority is already -999 in its defaults, so a
        # real headset always wins when one is connected.
        if ($settings.PSObject.Properties.Name -contains 'driver_null') {
            $settings.driver_null.enable = $true
        } else {
            $settings | Add-Member -NotePropertyName 'driver_null' -NotePropertyValue (
                [pscustomobject]@{ enable = $true })
        }

        $settings | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $configPath -Encoding UTF8
        Write-Host 'Null driver enabled.'

        if (-not $NoStart) {
            $vrstartup = Join-Path $vr 'bin/win64/vrstartup.exe'
            if (-not (Test-Path -LiteralPath $vrstartup)) { throw "Missing $vrstartup" }
            Write-Host 'Starting SteamVR ...'
            Start-Process -FilePath $vrstartup -WindowStyle Hidden

            for ($i = 1; $i -le 60; $i++) {
                Start-Sleep -Seconds 1
                if (Get-Process vrserver -ErrorAction SilentlyContinue) { break }
            }
            if (Get-Process vrserver -ErrorAction SilentlyContinue) {
                Write-Host "vrserver is up after ~$i s"
            } else {
                Write-Warning 'vrserver did not start.'
            }
        }
    }

    'Disable' {
        if ($running.Count -gt 0) {
            Write-Host ''
            Write-Host 'Stopping SteamVR ...'
            $running | Stop-Process -Force
            Start-Sleep -Seconds 2
        }

        if (Test-Path -LiteralPath $backupPath) {
            Copy-Item -LiteralPath $backupPath -Destination $configPath -Force
            Remove-Item -LiteralPath $backupPath -Force
            Write-Host 'Original config restored.'
        } else {
            Write-Host 'No backup to restore; leaving the config as it is.'
        }
    }
}
