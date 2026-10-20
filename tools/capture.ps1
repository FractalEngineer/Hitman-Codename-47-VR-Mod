#Requires -Version 5.1
<#
.SYNOPSIS
    Capture the running game's window to a PNG, and compare two captures.

.DESCRIPTION
    The old handoff recorded screenshots as impossible ("PrintWindow returns
    black, CopyFromScreen fails with 'the handle is invalid'"). Both parts of
    that are wrong, and the fix matters: PrintWindow with PW_RENDERFULLCONTENT
    returns the window's own contents, so the game can be watched *without*
    taking focus away from whatever the user is doing - and a desktop grab works
    too, but only shows whatever is on top.

    PrintWindow is therefore the default. It does not need the window to be
    visible or foreground, which is why it is the right tool for before/after
    comparisons: nothing about the capture disturbs the game.

    With -Compare, no capture happens: two PNGs are compared and a mean absolute
    per-channel difference is printed, which is the cheap way to answer "did that
    change the picture".

.PARAMETER Path
    PNG to write, or the first PNG when comparing.

.PARAMETER Compare
    Second PNG; when given, only a difference score is printed.

.PARAMETER ProcessName
    Process owning the window. Defaults to Hitman.

.PARAMETER NoFocus
    Only meaningful with -Screen: do not raise the window first.

.PARAMETER Screen
    Use a desktop grab of the window rectangle instead of PrintWindow. Needs the
    window to be on top; kept because it is the only way to see the composited
    desktop, which is occasionally the thing worth looking at.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [string]$Compare,
    [string]$ProcessName = 'Hitman',
    [switch]$NoFocus,
    [switch]$Screen
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @"
using System;
using System.Runtime.InteropServices;
public class H47Capture {
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc callback, IntPtr value);
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr value);

    public static IntPtr FindLargestWindow(uint processId) {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows(delegate(IntPtr h, IntPtr value) {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner != processId || !IsWindowVisible(h)) { return true; }
            RECT r;
            if (!GetClientRect(h, out r)) { return true; }
            long area = (long)(r.Right - r.Left) * (r.Bottom - r.Top);
            if (area > bestArea) { bestArea = area; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }
}
"@

if ($Compare) {
    $one = [System.Drawing.Bitmap]::FromFile($Path)
    $two = [System.Drawing.Bitmap]::FromFile($Compare)
    try {
        if ($one.Width -ne $two.Width -or $one.Height -ne $two.Height) {
            "size mismatch: $($one.Width)x$($one.Height) vs $($two.Width)x$($two.Height)"
            return
        }
        $total = 0.0
        $count = 0
        for ($y = 0; $y -lt $one.Height; $y += 4) {
            for ($x = 0; $x -lt $one.Width; $x += 4) {
                $p = $one.GetPixel($x, $y)
                $q = $two.GetPixel($x, $y)
                $total += [math]::Abs($p.R - $q.R) + [math]::Abs($p.G - $q.G) +
                          [math]::Abs($p.B - $q.B)
                $count += 3
            }
        }
        "mean absolute channel difference {0:N2} over {1} samples" -f ($total / $count), $count
    } finally {
        $one.Dispose()
        $two.Dispose()
    }
    return
}

$process = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
    Select-Object -First 1
if (-not $process) { throw "no $ProcessName window found" }

$handle = $process.MainWindowHandle
if ($handle -eq 0) {
    $handle = [H47Capture]::FindLargestWindow([uint32]$process.Id)
}
if ($handle -eq 0) { throw "no capturable window found for $ProcessName" }
if ($Screen -and -not $NoFocus) {
    if ([H47Capture]::IsIconic($handle)) { [void][H47Capture]::ShowWindow($handle, 9) }
    [void][H47Capture]::SetForegroundWindow($handle)
    Start-Sleep -Milliseconds 700
}

$rect = New-Object H47Capture+RECT
if ($Screen) {
    if (-not [H47Capture]::GetWindowRect($handle, [ref]$rect)) { throw 'GetWindowRect failed' }
    $width = $rect.Right - $rect.Left
    $height = $rect.Bottom - $rect.Top
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
    } finally {
        $graphics.Dispose()
    }
} else {
    if (-not [H47Capture]::GetClientRect($handle, [ref]$rect)) { throw 'GetClientRect failed' }
    $width = $rect.Right
    $height = $rect.Bottom
    $bitmap = New-Object System.Drawing.Bitmap $width, $height
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $dc = $graphics.GetHdc()
    try {
        # 2 = PW_RENDERFULLCONTENT. Without it an OpenGL window prints black.
        [void][H47Capture]::PrintWindow($handle, $dc, 2)
    } finally {
        $graphics.ReleaseHdc($dc)
        $graphics.Dispose()
    }
}

$directory = Split-Path -Parent $Path
if ($directory -and -not (Test-Path -LiteralPath $directory)) {
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
}
$bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

# A black or uniform grab is the failure mode worth catching rather than
# silently reporting success.
$check = [System.Drawing.Bitmap]::FromFile($Path)
$seen = @{}
for ($y = 0; $y -lt $check.Height; $y += 23) {
    for ($x = 0; $x -lt $check.Width; $x += 23) {
        $seen[$check.GetPixel($x, $y).ToArgb()] = $true
    }
}
$check.Dispose()
Write-Host ("captured {0} ({1}x{2}) via {3}; {4} distinct sampled colours" -f `
    $Path, $width, $height, $(if ($Screen) { 'screen' } else { 'PrintWindow' }), $seen.Count)
