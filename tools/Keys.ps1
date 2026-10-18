# Shared keyboard-injection helpers. Dot-source this; do not run it directly.
#
# The union must contain every INPUT variant, not just the keyboard one, or
# sizeof(INPUT) is wrong on 64-bit (32 instead of 40) and SendInput silently
# returns 0. That was the actual reason key injection kept doing nothing.

if (-not ('H47Input' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Threading;

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
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint uCode, uint uMapType);

    const uint KEYEVENTF_KEYUP = 0x0002;
    const uint KEYEVENTF_SCANCODE = 0x0008;

    public static int StructSize() { return Marshal.SizeOf(typeof(INPUT)); }

    // Games of this era read hardware scan codes through DirectInput, so an
    // event that carries only a virtual key is ignored by the game even though
    // GetAsyncKeyState reports the key as down. That is exactly the signature we
    // hit: keys proven to arrive, game acting as if nothing was pressed.
    static void Fill(ref KEYBDINPUT ki, ushort vk, bool up) {
        ki.wVk = vk;
        ki.wScan = (ushort)MapVirtualKey(vk, 0);
        ki.dwFlags = KEYEVENTF_SCANCODE | (up ? KEYEVENTF_KEYUP : 0);
    }

    static void Send(INPUT[] batch) {
        uint sent = SendInput((uint)batch.Length, batch, Marshal.SizeOf(typeof(INPUT)));
        if (sent != (uint)batch.Length) {
            throw new Exception("SendInput accepted " + sent + " of " + batch.Length +
                                " inputs (error " + Marshal.GetLastWin32Error() + ")");
        }
    }

    // Key down and key up go out in separate calls with a real gap between them.
    // Sending both in one burst is the same as a press that lasts zero
    // milliseconds, and a game that samples the keyboard once per frame can
    // never see it - it polls after the key is already up again.
    public static void Tap(ushort vk, bool shift, int holdMs) {
        int size = shift ? 2 : 1;

        var down = new INPUT[size];
        int i = 0;
        if (shift) { down[i].type = 1; Fill(ref down[i].u.ki, 0x10, false); i++; }
        down[i].type = 1; Fill(ref down[i].u.ki, vk, false); i++;
        Send(down);

        if (holdMs > 0) { Thread.Sleep(holdMs); }

        var up = new INPUT[size];
        i = 0;
        if (shift) { up[i].type = 1; Fill(ref up[i].u.ki, 0x10, true); i++; }
        up[i].type = 1; Fill(ref up[i].u.ki, vk, true); i++;
        Send(up);
    }

    public static void Tap(ushort vk, bool shift) { Tap(vk, shift, 60); }

    public static void Key(ushort vk) { Tap(vk, false, 60); }

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
