# Handoff

Copy this file's contents into a new chat to resume with full context.

---

## Mission

Finish a native 32-bit x86 VR mod for the Steam release of *Hitman: Codename 47*
(appid 6900, buildid 251800). Plan of record is `PLAN.md`, with a living
progress checklist at the top of it. Keep that checklist updated in the same
commit as the work.

Repository: `C:\Users\titan\Desktop\Github_Projects\Mine\Htman-Codename-47-VR-Mod`
(remote `FractalEngineer/Htman-Codename-47-VR-Mod`, branch `main`).

## Hard-won environment facts

- Game: `E:\SteamLibrary\steamapps\common\Hitman Codename 47`. All 11 hashes in
  `pins/build_251800.txt` match. The mod refuses to patch anything else.
- Toolchain: MSVC 14.44 x86 via VS 2022 Community (resolve it with
  `vswhere -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64`; there is
  also a VS "18" install *without* C++ tools). CMake 3.29, Ninja, Python 3.13.
- Ghidra 12.1.4 at `C:\Users\titan\tools\ghidra_12.1.4_PUBLIC`, JDK 21 at
  `C:\Users\titan\tools\jdk-21.0.12.1+1` (set `JAVA_HOME` before running it).
- GPU RTX 4070 Ti. Headset: Meta Quest 3 over Virtual Desktop.

### Launch chain (do not fight this)

- `Hitman.Exe` has an obfuscated Steam check. Launched directly it shows
  `Failed to find Steam.` and exits `0xFFFFFFFF`.
- The game uses the **legacy `steam.dll`**, not `steam_api.dll`, and only accepts
  a launch performed by the Steam client. Launch with:
  `steam.exe -applaunch 6900`.
- `runme.exe` is Valve's DRM stub; `runme.dat` contains `hitman.exe`.
- The game runs **fullscreen borderless at 800x600** (window 1682x1022, no
  `WS_CAPTION` on a 2560x1440 desktop). Adding `Window 1` to `Hitman.ini` did
  **not** make it windowed. While it runs, the user cannot use their desktop.
- Intro videos play on start; **ESC must be pressed to reach the menu.**
- The user reports the game sometimes exits on its own after ~16-30 s if the
  intro is not skipped.

## What is proven working

- **Proxy loader.** App-local `opengl32.dll` loads ahead of the system library and
  forwards **368/368** exports. It must re-export the *entire* surface: `gdi32`'s
  `ChoosePixelFormat`/`DescribePixelFormat`/`SetPixelFormat`/`GetPixelFormat`
  delegate to `opengl32!wgl*` at run time, so an import-scoped proxy breaks the
  game with `SelectPixelFormat failed`.
- **Presentation.** `SwapBuffers` comes from **gdi32**, not opengl32. It is hooked
  and firing; frames are counted.
- **Deferred gameplay hooks.** `LdrRegisterDllNotification` fires when
  `HitmanDlc.dlc` loads (it loads *after* the proxy starts) and installs hooks off
  the loader lock. Verified: module base `0x0E980000`, camera entry
  `0x0E9A64D0` = RVA `0x0264D0`, exactly as recorded. **The module relocates, so
  always use RVAs.**
- **MinHook** detours work in the shipping build.
- **OpenXR graphics bridge: PASSED headless.** Via SteamVR's null driver:
  instance created, `xrGetSystem` ok, 2 eyes at 1512x1680, legacy WGL context at
  GL 4.6.0, `xrCreateSession` over the OpenGL Win32 binding ok, session READY,
  `xrBeginSession` ok. Virtual Desktop's 32-bit runtime (`VirtualDesktopXR
  1.0.10`) also creates instances and advertises `XR_KHR_opengl_enable`, but
  needs an active stream for `xrGetSystem`.
- **Key injection verified at the OS level.** `SendInput` produces real key state
  (`GetAsyncKeyState` showed ESC down `0x8001`; INPUT is 40 bytes on x64).

## Current blocker: getting into gameplay

The camera hook is installed but **has never been observed firing**, because the
game has not reached a level. Everything else in Milestone 1 depends on this.

What has been tried and failed:

- Direct launches and `SteamAppId` env vars — the DRM refuses.
- Guessing console syntax: `startscene=C0_Training`, `startscene C0_Training`,
  `StartScene=C0_Training`. Wrong scene name and/or wrong syntax.
- Sending ESC + console commands by `SendInput` while refocusing. Keys are
  proven to reach the OS, but the game did not respond, and it exited early.

What is known statically:

- Scene names are **relative paths without the extension**. `HitmanDlc.dlc`
  holds the level-select table right next to `StateSelectLevel`:

  ```
  0x2299A0 C1_HongKong\C1_1
  0x2299B4 C0_Training\C0_1      <- training, the entry this project needs
  ```

  so the argument is `C0_Training\C0_1`, resolving to `C0_Training\C0_1.zip`. A
  second table at `0x2275xx` holds the bare names (`C0_1`, `C1_1`, ...) used to
  build the `_Pre` pre-scene variants, which is why bare `C0_1` looked right and
  was not.
- Console dispatcher: `System.dll` RVA **`0xE590`**, signature
  `__thiscall(void* this, char* commandLine, void* param2)`. The `startscene`
  branch calls a virtual function: `(vtbl+0x44)(2, param2, 0)`.
- Its help text, printed next to `Commandline:`, lists:
  `ReloadEngine, Exit, StartScene=[str]`, `PrintStatus`, `DoAllocCheck`,
  `Ingame: Packfile=[str], AutoDumpName=[str], LoadFilter=[.ext;.ext...],
  Include=[str], DefaultScene=[str], SoundBank=[str], AnimFile=[str],
  ProjectFile=[str]`.
- `ConsoleCmd` is recognised as an ini key but has **no handler** in the ini
  parser (`System.dll` RVA `0xEA00`) - dead end.
- Console output is **not** routed through `OutputDebugString` (both A and W are
  hooked; zero lines captured), and a GDI screen grab of the GL surface is
  **pure black**. There is no text channel out of the console yet.

## Console dispatcher, resolved (2026-10-18)

The blocker was "what is the exact console syntax", so the dispatcher's ABI was
read off the machine code instead of guessed:

- `System.dll` RVA `0xE590` is `int __thiscall(void* self, const char* name, const char* arg)`.
  Proven by the call site at RVA `0x104AD` (`mov ecx, edi` / `push` name /
  `push` arg / `call`) and the `ret 8` in its prologue.
- Its only caller (RVA `0x104AD`) is the line splitter: it terminates the
  command name at the first `=` **or** space in the line, so `startscene C0_1`
  and `startscene=C0_1` are equivalent, and the rest of the line is the argument.
- `self` is the interface singleton exported by `Globals.dll`
  (`?g_pSysInterface@@3PAVZSysInterface@@A`, RVA `0x3010`).
- The `startscene` branch calls `(*(this+0x59))->vtbl[0x44](2, sceneName, 0)`.

Because that ABI is now known, the mod does not need the in-game console at all:
`src/core/src/console_driver.cpp` detours RVA `0xE590` (logging every line the
engine executes, which is how a typed command can be observed) and can submit a
`startscene` line itself from the presentation hook. The scene comes from
`<game>\h47vr-scene.txt` or `H47VR_SCENE`; with neither set the driver is idle.

```powershell
./tools/iterate.ps1 -Scene C0_1 -CloseGame
```

MSVC rejects `__thiscall` on free functions, so the driver uses naked thunks:
a detour that preserves every register and tail-jumps to the MinHook
trampoline, and an invoke thunk that loads `ECX` and jumps to the same
trampoline. The original `ret 8` cleans the stack in both directions.

## Hook map (RVAs)

`HitmanDlc.dlc` (preferred base `0x0FCC0000`; it relocates):

| Symbol | RVA |
| --- | --- |
| `_ZCAMERA_BaseCamera` | `0x020E80` |
| `_ZCAMERA_MouseConCam` | `0x0264D0` |
| `_ZCAMERA_MainCameraControl` | `0x023940` |
| `_ZCAMERA_NewFollowCam2` | `0x029700` |
| `_ZCAMERA_ScopeCamera` | `0x02D060` |
| `_ZLIST_SortCameraControls` | `0x023430` |
| `_ZGEOM_AdoptVisibility` | `0x159050` |
| `_ZLIST_CheckVisible` | `0x14DE30` |
| `_ZITEM_FireShot` | `0x0EBA30` |
| `_ZGEOM_BasePickupTarget` | `0x00D980` |
| `_ZGEOM_ItemActivate` | `0x020240` |

`Globals.dll`: `?g_pSysInput@@3PAVZSysInput@@A` RVA `0x003014`.
`System.dll`: `RunEngine` `0xA790`, `EditPrint` `0xA770`.
`HitmanDlc.dlc` exports **343** named symbols; the full table is
`docs/re/hitmandlc_exports.txt`.

`RenderOpenGL.dll` (loaded at preferred base `0x0FB70000` in testing):

| Target | RVA | Measured |
| --- | --- | --- |
| present (only caller of `SwapBuffers`) | `0x0048F0` | ~1 per frame |
| frame draw (`RenderWintelOpenGLDraw.cpp`) | `0x003680` | **~17-18 per frame**, i.e. per pass |
| object draw (`glDrawElements`, model matrix) | `0x006590` | 0 at the menu |
| object draw variant (`glCullFace`) | `0x006270` | 0 at the menu |
| 2D UI passes (`glOrtho` + `glViewport`) | `0x004930`, `0x005300`, `0x005A10`, `0x005F20`, `0x002EC0` | menu layers |
| texture upload | `0x0015E0`, `0x013000`, `0x013700`, `0x014170` | |

The eye loop belongs at **`0x0048F0`** (once per frame). `0x003680` is per-pass
and would render ~18 partial eyes. **Gap:** `glFrustum` is only called from code
Ghidra has not defined as a function; that projection setup must be recovered
before asymmetric per-eye projections can be added.

## Tooling

```powershell
./tools/fingerprint.ps1          # verify the 11 pins
./tools/build.ps1                # x86 build (Ninja + vswhere-selected MSVC)
./tools/install-mod.ps1          # proxy + OpenGL renderer + manifest (reversible)
./tools/uninstall-mod.ps1        # restore + re-verify pins
./tools/iterate.ps1              # build, install, steam launch, tail log
./tools/enter-game.ps1           # launch, ESC burst, optional console commands, closes game
./tools/send-keys.ps1            # send vk:/text: sequences (uses tools/Keys.ps1)
./tools/steamvr-headless.ps1     # -Action Enable|Disable|Status (null HMD)
./tools/ghidra-analyze.ps1       # headless analysis + anchor extraction
python tools/peutils.py <dll>    # list exports with RVAs
```

Ghidra scripts in `tools/ghidra/`: `ExtractAnchors.java`, `DumpReferences.java`
(GL import call sites), `DecompileXrefs.java` (decompile whatever references a
string - syntax: `<outDir> <needle>...`). Run them against the existing project:

```
analyzeHeadless ghidra_project h47vr -process System.dll -noanalysis
  -scriptPath tools/ghidra -postScript DecompileXrefs.java tools/_ghidra_out <needle>
```

Vendored: `third_party/openxr` (loader 1.1.65, x86) and `third_party/minhook`
(v1.3.4). Built by `tools/fetch-openxr.ps1` / `tools/fetch-minhook.ps1`.

## Rules that save time

- Anything touching `E:\`, `%APPDATA%`, or `.git` needs
  `sandbox_permissions: require_escalated`. `git add`/`commit` also need it.
- PowerShell `-File` splits comma lists into separate argv entries. Split inside
  the script (`$x = @($x | % { $_ -split ',' })`).
- The game must be closed before install/uninstall.
- The user's desktop is unusable while the game runs (fullscreen 800x600). Close
  the game when done; do not launch repeatedly without saying so.
- Never call `OutputDebugString` from a detour without a `LoggerActive()` guard -
  the logger calls that API and the mutex is non-recursive (this deadlocked the
  game once).
- The gameplay exports are `_name` with no `@N`, i.e. **cdecl**, which makes an
  unknown arity safe to forward. Renderer internals are likely `__thiscall`; use
  convention-agnostic naked thunks that preserve flags and args.

## Immediate next steps

1. **Get into gameplay.** The dispatcher ABI is resolved and the mod now drives
   it directly: run `./tools/iterate.ps1 -Scene C0_1` and read the summary. Success
   is `max objectDrawA > 0`. If the scene name is rejected the dispatcher returns
   0 and logs "Unrecognized command"; if it is accepted but nothing renders,
   check whether `g_pSysInterface` was still null at the attempt.
2. Once a level loads (`objectDrawA > 0` in the log), confirm
   `_ZCAMERA_MouseConCam` fires and with what arguments; then modify and verify
   the camera moves. That closes the Milestone 1 gate.
3. Milestone 2: eye loop at `0x0048F0`; recover the `glFrustum` caller for
   asymmetric projections; leave the five 2D passes out of the per-eye loop.
4. Install x32dbg to confirm calling conventions instead of deducing them.
