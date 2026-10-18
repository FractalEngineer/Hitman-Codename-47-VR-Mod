<#
.SYNOPSIS
    Send a key sequence to the running game window.

.DESCRIPTION
    The engine exposes a console, opened with the backtick key, that accepts
    text commands. This focuses the game window and types a sequence into it, so
    the console can be driven without a human at the keyboard.

    Each element of -Sequence is either
      vk:<n>     a virtual-key code, decimal or 0x-prefixed hex
      text:<s>   literal text, typed with the shift state each character needs

    Examples:
      # open the console, list the commands, close it
      ./tools/send-keys.ps1 -Sequence 'vk:0xC0','text:<commands>','vk:0x0D'

      # open the console and try to start the training level
      ./tools/send-keys.ps1 -Sequence 'vk:0xC0','text:startscene=C0_Training','vk:0x0D'

.PARAMETER Sequence
    Steps to send, in order.

.PARAMETER StepDelayMs
    Pause between steps, to let the game react.

.PARAMETER KeyDelayMs
    Pause between individual keystrokes.

.PARAMETER ProcessName
    Target process. Defaults to Hitman.

.EXAMPLE
    ./tools/send-keys.ps1 -Sequence 'vk:0xC0','text:<commands>','vk:0x0D'
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

# Accept both "-Sequence a,b,c" (one literal string when invoked via -File) and
# a real PowerShell array.
$Sequence = @($Sequence | ForEach-Object { $_ -split ',' } | Where-Object { $_ })

Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class Keys {
    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT {
        public uint type;
        public InputUnion u;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct InputUnion {
        [FieldOffset(0)] public KEYBDINPUT ki;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk;
        public ushort wScan;
        public uint dwFlags;
        public uint time;
        public IntPtr dwExtraInfo;
    }

    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] inputs, int cbSize);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern short VkKeyScan(char ch);

    const uint KEYEVENTF_KEYUP = 0x0002;

    static void Tap(ushort vk, bool shift) {
        var inputs = new INPUT[shift ? 4 : 2];
        int i = 0;
        if (shift) {
            inputs[i].type = 1; inputs[i].u.ki.wVk = 0x10; i++;
        }
        inputs[i].type = 1; inputs[i].u.ki.wVk = vk; i++;
        inputs[i].type = 1; inputs[i].u.ki.wVk = vk; inputs[i].u.ki.dwFlags = KEYEVENTF_KEYUP; i++;
        if (shift) {
            inputs[i].type = 1; inputs[i].u.ki.wVk = 0x10; inputs[i].u.ki.dwFlags = KEYEVENTF_KEYUP; i++;
        }
        SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static void Key(ushort vk) { Tap(vk, false); }

    public static void Text(string s) {
        foreach (char ch in s) {
            short scan = VkKeyScan(ch);
            if (scan == -1) continue;
            bool shift = (scan & 0x0100) != 0;
            Tap((ushort)(scan & 0xFF), shift);
        }
    }
}
'@

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
if (-not $process) { throw "No $ProcessName window found. Is the game running?" }

$window = $process.MainWindowHandle
Write-Host ("target: pid {0}, window '{1}'" -f $process.Id, $process.MainWindowTitle)

[void][Keys]::ShowWindow($window, 9)   # SW_RESTORE
[void][Keys]::SetForegroundWindow($window)
Start-Sleep -Milliseconds 500

if ([Keys]::GetForegroundWindow() -ne $window) {
    Write-Warning 'the game window is not foreground; keystrokes may go elsewhere'
}

foreach ($step in $Sequence) {
    if ($step -like 'vk:*') {
        $value = $step.Substring(3)
        $vk = if ($value -like '0x*') { [Convert]::ToInt32($value, 16) } else { [int]$value }
        [Keys]::Key([ushort]$vk)
        Write-Host ("  vk 0x{0:X2}" -f $vk)
        Start-Sleep -Milliseconds $KeyDelayMs
    } elseif ($step -like 'text:*') {
        $text = $step.Substring(5)
        [Keys]::Text($text)
        Write-Host "  text '$text'"
        Start-Sleep -Milliseconds $KeyDelayMs
    } else {
        throw "Unknown step '$step'. Use vk:<n> or text:<s>."
    }
    Start-Sleep -Milliseconds $StepDelayMs
}

Write-Host 'sent.'
