<#
.SYNOPSIS
    Launch the game, skip the intro immediately, and drive the console.

.DESCRIPTION
    The intro videos only accept the ESC skip while they are on screen, so this
    runs the whole sequence inside one process with the P/Invoke compiled once:
    launch, poll for the window, press ESC the moment it appears, then open the
    console and run a command.

    Doing it through separate script invocations loses the window: PowerShell
    start-up plus Add-Type compilation costs enough that the intro is gone.

.PARAMETER ConsoleCommand
    Text typed into the console after it is opened.

.PARAMETER KeepOpen
    Leave the console open at the end instead of closing it.

.EXAMPLE
    ./tools/enter-game.ps1
    ./tools/enter-game.ps1 -ConsoleCommand 'startscene=C0_Training'
#>
[CmdletBinding()]
param(
    [string]$ConsoleCommand = '<commands>',
    [int]$WindowTimeoutSeconds = 120,
    [switch]$KeepOpen,
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'

Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class H47Keys {
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public U u; }
    [StructLayout(LayoutKind.Explicit)] public struct U { [FieldOffset(0)] public K ki; }
    [StructLayout(LayoutKind.Sequential)] public struct K {
        public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr extra;
    }
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int cb);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern short VkKeyScan(char ch);

    static void Tap(ushort vk, bool shift) {
        var a = new INPUT[shift ? 4 : 2];
        int i = 0;
        if (shift) { a[i].type = 1; a[i].u.ki.wVk = 0x10; i++; }
        a[i].type = 1; a[i].u.ki.wVk = vk; i++;
        a[i].type = 1; a[i].u.ki.wVk = vk; a[i].u.ki.dwFlags = 2; i++;
        if (shift) { a[i].type = 1; a[i].u.ki.wVk = 0x10; a[i].u.ki.dwFlags = 2; i++; }
        SendInput((uint)a.Length, a, Marshal.SizeOf(typeof(INPUT)));
    }
    public static void Key(ushort vk) { Tap(vk, false); }
    public static void Text(string s) {
        foreach (char c in s) {
            short scan = VkKeyScan(c);
            if (scan == -1) continue;
            Tap((ushort)(scan & 0xFF), (scan & 0x0100) != 0);
        }
    }
}
'@

function Get-GameWindow {
    Get-Process -Name Hitman -ErrorAction SilentlyContinue |
        Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
}

if (-not $NoLaunch) {
    if (Get-Process -Name Hitman -ErrorAction SilentlyContinue) {
        throw 'The game is already running. Close it first.'
    }
    Write-Host 'launching via Steam ...'
    Start-Process -FilePath 'C:\Program Files (x86)\Steam\steam.exe' -ArgumentList '-applaunch', '6900'
}

# Poll for the *process*, not the window. Waiting for MainWindowHandle loses the
# intro: the window only becomes enumerable once rendering is already underway,
# and the skip window is short.
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$deadline = $clock.Elapsed.TotalSeconds + $WindowTimeoutSeconds
$proc = $null
while ($clock.Elapsed.TotalSeconds -lt $deadline) {
    $proc = Get-Process -Name Hitman -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($proc) { break }
    Start-Sleep -Milliseconds 50
}
if (-not $proc) { throw "No game process appeared within $WindowTimeoutSeconds s." }
Write-Host ("process up after {0:N0} ms (pid {1})" -f $clock.Elapsed.TotalMilliseconds, $proc.Id)

# Press ESC continuously for ~2 seconds. Once the menu is up ESC does nothing,
# so a burst is safe and is far more robust than timing three discrete presses:
# the engine polls key state, and this lands even before the window is focusable.
Start-Sleep -Milliseconds 100
for ($i = 1; $i -le 14; $i++) {
    [H47Keys]::Key(0x1B)
    if ($i -in 1, 14) {
        Write-Host ("  ESC burst {0}/14 at {1:N0} ms" -f $i, $clock.Elapsed.TotalMilliseconds)
    }
    Start-Sleep -Milliseconds 150
}

$game = $null
while ($clock.Elapsed.TotalSeconds -lt $deadline) {
    $game = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
    if ($game -and $game.MainWindowHandle -ne 0) { break }
    Start-Sleep -Milliseconds 50
}
if (-not $game) { throw 'the game exited while skipping the intro' }
[void][H47Keys]::ShowWindow($game.MainWindowHandle, 9)
[void][H47Keys]::SetForegroundWindow($game.MainWindowHandle)
Write-Host ("window ready at {0:N0} ms: '{1}'" -f $clock.Elapsed.TotalMilliseconds, $game.MainWindowTitle)
Start-Sleep -Seconds 2

Write-Host 'opening the console ...'
[H47Keys]::Key(0xC0)
Start-Sleep -Milliseconds 600
[H47Keys]::Text($ConsoleCommand)
Start-Sleep -Milliseconds 400
[H47Keys]::Key(0x0D)
Write-Host "  ran: $ConsoleCommand"

Start-Sleep -Seconds 3

if (-not $KeepOpen) {
    [H47Keys]::Key(0xC0)
    Start-Sleep -Milliseconds 300
}

$alive = Get-Process -Id $game.Id -ErrorAction SilentlyContinue
if ($alive) {
    Write-Host ("game alive: pid {0}, window '{1}'" -f $alive.Id, $alive.MainWindowTitle)
} else {
    Write-Host 'game exited'
}

$logDir = 'E:\SteamLibrary\steamapps\common\Hitman Codename 47\h47vr-logs'
$log = Get-ChildItem -LiteralPath $logDir -Filter *.log -ErrorAction SilentlyContinue |
    Sort-Object LastWriteTime -Descending | Select-Object -First 1
if ($log) {
    $stream = [System.IO.File]::Open($log.FullName,
        [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $reader = New-Object System.IO.StreamReader($stream)
    $text = $reader.ReadToEnd(); $reader.Dispose(); $stream.Dispose()
    $engine = @($text -split "`n" | Where-Object { $_ -match 'engine:' })
    Write-Host ("engine lines captured: {0}" -f $engine.Count)
    $engine | Select-Object -First 60
}
