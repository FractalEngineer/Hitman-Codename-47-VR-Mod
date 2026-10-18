# Shared keyboard-injection helpers. Dot-source this; do not run it directly.
#
# The union must contain every INPUT variant, not just the keyboard one, or
# sizeof(INPUT) is wrong on 64-bit (32 instead of 40) and SendInput silently
# returns 0. That was the actual reason key injection kept doing nothing.

if (-not ('H47Input' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;

public static class H47Input {
    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT {
        public int dx; public int dy; public uint mouseData;
        public uint dwFlags; public uint time; public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo;
    }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT {
        public uint type;
        public INPUTUNION u;
    }

    [DllImport("user32.dll", SetLastError = true)]
    public static extern uint SendInput(uint nInputs, INPUT[] inputs, int cbSize);

    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern short VkKeyScan(char ch);

    const uint KEYEVENTF_KEYUP = 0x0002;

    public static int StructSize() { return Marshal.SizeOf(typeof(INPUT)); }

    public static void Tap(ushort vk, bool shift) {
        var a = new INPUT[shift ? 4 : 2];
        int i = 0;
        if (shift) { a[i].type = 1; a[i].u.ki.wVk = 0x10; i++; }
        a[i].type = 1; a[i].u.ki.wVk = vk; i++;
        a[i].type = 1; a[i].u.ki.wVk = vk; a[i].u.ki.dwFlags = KEYEVENTF_KEYUP; i++;
        if (shift) { a[i].type = 1; a[i].u.ki.wVk = 0x10; a[i].u.ki.dwFlags = KEYEVENTF_KEYUP; i++; }

        uint sent = SendInput((uint)a.Length, a, Marshal.SizeOf(typeof(INPUT)));
        if (sent != (uint)a.Length) {
            throw new Exception("SendInput accepted " + sent + " of " + a.Length +
                                " inputs (error " + Marshal.GetLastWin32Error() + ")");
        }
    }

    public static void Key(ushort vk) { Tap(vk, false); }

    public static void Text(string s) {
        foreach (char c in s) {
            short scan = VkKeyScan(c);
            if (scan == -1) { continue; }
            Tap((ushort)(scan & 0xFF), (scan & 0x0100) != 0);
        }
    }
}
'@
}

function Get-GameProcess {
    Get-Process -Name Hitman -ErrorAction SilentlyContinue | Select-Object -First 1
}

function Focus-GameWindow {
    param($Process)
    if ($Process.MainWindowHandle -eq 0) { return $false }
    [void][H47Input]::ShowWindow($Process.MainWindowHandle, 9)
    [void][H47Input]::SetForegroundWindow($Process.MainWindowHandle)
    Start-Sleep -Milliseconds 150
    return ([H47Input]::GetForegroundWindow() -eq $Process.MainWindowHandle)
}
