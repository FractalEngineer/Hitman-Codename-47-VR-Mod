<#
.SYNOPSIS
    Send a key sequence to the running game window.

.DESCRIPTION
    Each element of -Sequence is either
      vk:<n>     a virtual-key code, decimal or 0x-prefixed hex
      text:<s>   literal text, typed with the shift state each character needs

    Uses tools/Keys.ps1, which has the correctly sized INPUT union. An earlier
    version declared only the keyboard variant, so SendInput was handed the
    wrong cbSize on 64-bit and silently did nothing.

.EXAMPLE
    ./tools/send-keys.ps1 -Sequence 'vk:0xC0,text:<commands>,vk:0x0D'
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string[]]$Sequence,
    [int]$StepDelayMs = 350,
    [int]$KeyDelayMs = 40,
    [string]$ProcessName = 'Hitman'
)

$ErrorActionPreference = 'Stop'

# Accept both "-Sequence a,b,c" (one literal string via -File) and an array.
$Sequence = @($Sequence | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
. (Join-Path $PSScriptRoot 'Keys.ps1')

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "No $ProcessName window found. Is the game running?" }

Write-Host ("target: pid {0}, window '{1}', INPUT is {2} bytes" -f
    $process.Id, $process.MainWindowTitle, [H47Input]::StructSize())

if (-not (Focus-GameWindow -Process $process)) {
    Write-Warning 'the game window is not foreground; keystrokes may go elsewhere'
}

foreach ($step in $Sequence) {
    if ($step -like 'vk:*') {
        $value = $step.Substring(3)
        $vk = if ($value -like '0x*') { [Convert]::ToInt32($value, 16) } else { [int]$value }
        [H47Input]::Key([ushort]$vk)
        Write-Host ("  vk 0x{0:X2}" -f $vk)
        Start-Sleep -Milliseconds $KeyDelayMs
    } elseif ($step -like 'text:*') {
        $text = $step.Substring(5)
        [H47Input]::Text($text)
        Write-Host "  text '$text'"
        Start-Sleep -Milliseconds $KeyDelayMs
    } else {
        throw "Unknown step '$step'. Use vk:<n> or text:<s>."
    }
    Start-Sleep -Milliseconds $StepDelayMs
}

Write-Host 'sent.'
