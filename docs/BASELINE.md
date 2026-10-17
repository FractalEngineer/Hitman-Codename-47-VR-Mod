# Baseline and Integration Points

Milestone 1 status as of 2026-10-18. Everything below was verified on this
machine unless marked as an assumption.

## Verified environment

| Item | Finding |
| --- | --- |
| Install | `E:\SteamLibrary\steamapps\common\Hitman Codename 47` |
| Steam app / build | appid `6900`, buildid `251800` (from `appmanifest_6900.acf`) |
| Build pins | all 11 modules match - see `pins/build_251800.txt` |
| Architecture | every module is i386 (`machine 0x014c`), confirmed by parsing each PE |
| Current renderer | `DrawDll RenderD3D.dll` in `Hitman.ini` |
| Second renderer | `RenderOpenGL.dll` works (see Renderer results) |
| Toolchain | MSVC 14.44.35207 via VS 2022 Community, CMake 3.29.2, Ninja, Python 3.13 |
| Analysis | Ghidra 12.1.4 (`C:\Users\titan\tools\ghidra_12.1.4_PUBLIC`) with Temurin JDK 21.0.12.1 |
| Missing | x32dbg, MinHook, an `openxr_loader.dll` of any architecture |

Corrections to the plan's original assumptions:

- The machine also has a VS "18" install without the C++ tools. `tools/build.ps1`
  resolves the toolchain through
  `vswhere -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`, so it
  always selects the VS 2022 install that actually has them.
- No `openxr_loader.dll` exists anywhere on the machine, including under
  `System32` and `SysWOW64`. The x86 loader must be vendored or built.

## Launch chain

This is the most important finding of the milestone, and it changes the
development workflow.

```
Steam client -> runme.exe -> Hitman.Exe -> RenderOpenGL.dll -> opengl32.dll
                (DRM stub)   (thin host)   (renderer)         (system)
```

- `Hitman.Exe` implements a Steam presence check with obfuscated strings.
  Launched directly it shows an "Application error" dialog containing
  `Failed to find Steam.` and exits with `0xFFFFFFFF`.
- Running it inside a restricted context (the agent sandbox) produces the same
  dialog even with Steam running, because the child cannot reach Steam's IPC.
  Testing therefore has to happen in the normal desktop context.
- `runme.exe` is Valve's DRM launcher: `runme.dat` contains the literal text
  `hitman.exe`, and its PDB path leaks `u:\valve_main\src\utils\runme`. Started
  by hand it exits `0` without launching the game; it expects Steam to start it.
- Consequence: launching the git-ignored `.gamedev/Hitman` copy is **not** a
  viable test path. The only working launch is through Steam, from the Steam
  install directory.

## Loader

The mod loads as an app-local `opengl32.dll` in the game directory. Windows
searches the application directory before the system directories, so the game
loads our module in place of the system renderer. Every export forwards to
`C:\Windows\system32\opengl32.dll` by absolute path, loaded lazily on first use
rather than from `DllMain`.

Confirmed in-process:

```
OPENGL32.dll      E:\SteamLibrary\steamapps\common\Hitman Codename 47\OPENGL32.dll
opengl32.dll      C:\Windows\system32\opengl32.dll
RenderOpenGL.dll  ...\RenderOpenGL.dll
```

and in the log:

```
[info ] process architecture: x86 (WOW64)
[info ] pin  ok   Hitman.Exe              (all 11 pins)
[info ] supported build 251800 confirmed
[info ] proxy: forwarded 368 of 368 opengl32 exports from C:\Windows\system32\opengl32.dll
```

### Export surface

An initial proxy exported only the 67 functions imported by `RenderOpenGL.dll`.
That broke the game with
`OpenGL: SelectPixelFormat failed. Hardware acceleration not supported.`

The cause is that `gdi32`'s `ChoosePixelFormat`, `DescribePixelFormat`,
`SetPixelFormat` and `GetPixelFormat` reach the OpenGL client library at run time
through `opengl32!wglChoosePixelFormat`, `wglDescribePixelFormat`,
`wglSetPixelFormat` and `wglGetPixelFormat`. Shadowing the DLL without
re-exporting those broke pixel format selection.

The proxy now re-exports the entire 368-symbol surface of the system
`opengl32.dll` (368 named exports, zero ordinal-only).
`tools/gen_opengl32_proxy.py` generates it from the real export table, so it
cannot drift.

### Presentation

`SwapBuffers` is **not** an opengl32 export. `RenderOpenGL.dll` imports it, along
with `SelectPalette`, `DescribePixelFormat`, `GetDeviceCaps`, `SetPixelFormat`,
`GetStockObject`, `RealizePalette`, `CreatePalette` and `GetPixelFormat`, from
**`gdi32.dll`**. Intercepting presentation therefore means hooking `gdi32`, not
the proxy.

## Renderer results

| Renderer | Result |
| --- | --- |
| Direct3D (as shipped) | Runs. 92 MB working set, title `Direct3D`. |
| OpenGL, no proxy | Runs. 180 MB, title `OpenGL`, system `OPENGL32.dll`. |
| OpenGL, 67-export proxy | Fatal error at pixel format selection. |
| OpenGL, 368-export proxy | Runs. 125 MB, title `OpenGL`, our proxy ahead of the system one. |

Both renderers work. The OpenGL path is the supported investigation route and is
now validated end to end with the proxy installed.

## Third-party modules

`DSOUND.dll` (19,968 bytes) and `scripts/h1w.asi` are present and pinned, so a
changed copy is detected. `DSOUND.dll` is loaded by the game in place of the
system `dsound.dll`, with the system copy loaded alongside it. Neither is hooked
or replaced. The widescreen plugin's effect has not been measured yet.

## Development workflow

Steam must perform the launch, so the dev loop installs into the Steam directory
and removes itself afterwards:

```powershell
./tools/build.ps1                 # build 32-bit artifacts
./tools/install-mod.ps1           # proxy + OpenGL renderer + manifest
# launch the game from Steam, then inspect <game>\h47vr-logs
./tools/uninstall-mod.ps1         # restore, then re-verify the pins
```

`install-mod.ps1` refuses to run if an `opengl32.dll` already exists, backs up
`Hitman.ini` once, and writes `h47vr-install.json`. `uninstall-mod.ps1` deletes
exactly what was added, restores the ini, removes the log directory, and re-runs
the fingerprint check, so a successful uninstall proves the commercial copy is
intact. Both directions were exercised; the pins matched afterwards.

The git-ignored `.gamedev/Hitman` copy remains useful for static analysis and
diffing, but it cannot be launched.

## Open items carried forward

- Ghidra has not been run against the binaries yet, so no function addresses are
  known and the hook map is still targets-only.
- The OpenXR graphics bridge gate has not been attempted: no x86 loader is
  present and the runtime's in-process OpenGL requirements are unverified.
- The widescreen plugin's effect on the OpenGL path is unmeasured.
