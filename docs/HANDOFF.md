# Handoff

Resume work from here. This supersedes every earlier handoff.

## Mission

Native 32-bit x86 VR mod for the Steam release of *Hitman: Codename 47*
(appid 6900, buildid 251800). Plan of record is `PLAN.md`, with a living
progress checklist at the top of it - **update that checklist in the same commit
as the work**.

Repo: `C:\Users\titan\Desktop\Github_Projects\Mine\Htman-Codename-47-VR-Mod`
(remote `FractalEngineer/Htman-Codename-47-VR-Mod`, branch `main`).

## Where things stand (2026-10-19)

**The long-standing blocker is gone.** The game can be driven into a level with
no keyboard input, and `_ZCAMERA_MouseConCam` has now been observed firing in a
loaded level - the Milestone 1 signal this project spent weeks chasing.

The game's own level start is

```c
world->vtbl[0x88](levelName, 0, 0);
```

read out of `FUN_0fd3b609` (`HitmanDlc.dlc` RVA `0x7B609`), which reads the
string property `strPreScene`, maps the `_Pre` name to the plain level name
through a 13-entry `strcmp` chain, logs `Prescene push %s`, and calls that slot.
`ScenePushLevel` in `src/core/src/scene_probe.cpp` reproduces it exactly.

Confirmed by the user: the push lands **exactly at the start of the level**. A
scripted cutscene plays before control - **ESC skips it**, otherwise it ends on
its own and control is handed over. There is no skip string anywhere in the
module (cutscenes are `CutSequence` data on a fixed timeline), so a test loop
should push, wait for the cutscene, then drive the camera.

## Working commands

```powershell
# deploy the current build + launch via Steam (the deploy step matters: Steam
# runs whatever is in the game directory, so a bare launch runs the OLD build)
./tools/enter-level.ps1 -BurstSeconds 0 -KeepOpen

# into a level, any time after that, no relaunch and no keyboard
./tools/drive.ps1 -Scene 'push:C0_Training\C0_1'

# full loop: build, install, launch, watch the log, close
./tools/iterate.ps1 -Scene C0_1 -CloseGame
```

Every level is reachable: `C0_Training\C0_1`, `C1_HongKong\C1_1..C1_4`,
`C2_RotterdamHarbor\C2_1/C2_2`, `C3_BudapestHotel\C3_1`,
`C4_ColombianRainforest\C4_1..C4_3`, `C5_Sanitarium\C5_1/C5_2`.

The request file `<game>\h47vr-go.txt` is polled at frame boundaries and
consumed on sight. Its payload decides what happens:

| Payload | Meaning |
| --- | --- |
| `push:<level>` | **the working level load** (game's own slot) |
| `<mode>:<name>` | call the EngineData command entry directly with that mode |
| anything else | console `startscene <line>` (dead end, kept for reference) |

## Environment

- Game: `E:\SteamLibrary\steamapps\common\Hitman Codename 47`. All 11 hashes in
  `pins/build_251800.txt` match; the mod refuses to patch anything else.
- Toolchain: MSVC 14.44 x86 via VS 2022 Community (`vswhere -requires
  Microsoft.VisualStudio.Component.VC.Tools.x86.x64`), CMake 3.29, Ninja,
  Python 3.13.
- Ghidra 12.1.4 at `C:\Users\titan\tools\ghidra_12.1.4_PUBLIC`, JDK 21 at
  `C:\Users\titan\tools\jdk-21.0.12.1+1` (set `JAVA_HOME` before running it;
  Ghidra needs `require_escalated` because it writes under `%APPDATA%`).
- GPU RTX 4070 Ti. Headset: Meta Quest 3 over Virtual Desktop.
- The game is currently **windowed** (a bare `Window` line in `Hitman.ini`), so
  the user can see Codex while it runs.

### Launch chain (do not fight this)

- `Hitman.Exe` has an obfuscated Steam check; launched directly it says
  `Failed to find Steam.` and exits `0xFFFFFFFF`. The game uses the legacy
  `steam.dll` and only accepts a launch by the Steam client:
  `steam.exe -applaunch 6900`.
- The engine reads `Hitman.ini` **before** the renderer loads, so those lines
  are processed before the mod exists and cannot be observed from inside.
  `Anisotropy`, `ShadowDetail`, `TextureResolution` reach the EngineData
  entry as `mode=1`, which is how that entry was identified.

## What is proven working

- **Proxy loader.** App-local `opengl32.dll` forwards 368/368 exports. It must
  re-export the *entire* surface: `gdi32`'s pixel-format calls delegate to
  `opengl32!wgl*` at run time, so an import-scoped proxy breaks the game with
  `SelectPixelFormat failed`.
- **Presentation.** `SwapBuffers` comes from **gdi32**; hooked and counting.
- **Deferred hooks** off the loader lock via `LdrRegisterDllNotification`.
- **Re-arming after module reloads.** Level transitions reload the engine's
  modules and discard every patch. Loads now raise a flag and the frame boundary
  re-installs renderer, camera, console and scene-probe hooks. Module load/unload
  history is recorded under the loader lock and flushed from the render thread.
- **Level entry, keyboard-free.** See above. Verified end to end by the user.
- **Gameplay camera observed.** `_ZCAMERA_MouseConCam` fires (rarely - it is
  camera setup, not the per-frame update), with a stable first argument.
- **Console/config dispatcher reached in-process.** `System.dll` RVA `0xE590`
  is `__thiscall(object, name, arg)` with `ret 8`, verified from the machine code
  at its call site (RVA `0x104AD`) and its prologue. It splits a line at the
  first `=` or space. `self` is `Globals.dll`'s
  `?g_pSysInterface@@3PAVZSysInterface@@A` at RVA `0x3010`.

## Reverse-engineering map

Modules relocate, so **always use RVAs**. Preferred bases in brackets.

`HitmanDlc.dlc` [`0x0FCC0000`]:

| Item | RVA |
| --- | --- |
| Level start `FUN_0fd3b609` (calls `world->vtbl[0x88](level,0,0)`) | `0x7B609` |
| Level -> `_Pre` mapping chain (13 `strcmp` + level names) | `0x7B650`..`0x7B850` |
| `_ZWINGROUP_StatePreScene` descriptor / its handler | `0x7B0A0` / `FUN_0fd3b0d0` |
| `_ZWINGROUP_StateSelectLevel` descriptor / handler | `0x7D040` / `FUN_0fd3d070` |
| `_ZWINGROUP_StateMain` descriptor / handler | `0x79280` / `FUN_0fd392b0` |
| `_ZWINGROUP_StateIntro` descriptor / handler | `0x608A0` / `FUN_0fd208d0` |
| `_ZGEOM_DemoMode` (front-end screen whose script is `ZGEOMREF StateIntro`) | `0x26D0` |
| Level-name table (data, plain names) | file `0x2299A0`..`0x2299B4` |
| `_ZCAMERA_MouseConCam` | `0x0264D0` |
| `_ZCAMERA_BaseCamera` / `_MainCameraControl` / `_NewFollowCam2` / `_ScopeCamera` | `0x020E80` / `0x023940` / `0x029700` / `0x02D060` |
| `_ZLIST_SortCameraControls` | `0x023430` |
| `_ZGEOM_AdoptVisibility` / `_ZLIST_CheckVisible` | `0x159050` / `0x14DE30` |
| `_ZITEM_FireShot` | `0x0EBA30` |
| `_ZGEOM_BasePickupTarget` / `_ZGEOM_ItemActivate` | `0x00D980` / `0x020240` |

`Globals.dll` [`0x0FFF0000`]: `?g_pSysInterface...` `0x3010`,
`?g_pSysInput...` `0x3014`.

`System.dll` [`0x0FFA0000`]: `RunEngine` `0xA790`, `EditPrint` `0xA770`,
console/config dispatcher `0xE590`, its line-splitting caller `0x104AD`.

`EngineData.dll` [`0x0FF60000`]: RVA `0xEF60` is the **settings/clip** entry
(`EngineData.cpp`; switches on `Cams`, `Hira`, `Dir`, `HiraClip`, `DirClip`);
the world vtable is at RVA `0x30298`.

`RenderOpenGL.dll` (loads at `0x0FB70000`): present `0x0048F0` (~1/frame, the
only caller of `SwapBuffers`), frame draw `0x003680` (~17-22 passes/frame),
`glDrawElements` candidates `0x006590` and `0x006270`, 2D UI passes `0x004930`,
`0x005300`, `0x005A10`, `0x005F20`, `0x002EC0`.

Front end structure: every screen is an **exported wing group**
(`_ZWINGROUP_*`) built by a descriptor that stores a script string, a handler, a
float and an id, and screens are switched **by name** via
`vtbl[0x49c]("StateSelectLevel")`. Front-end data is a name/value registry
reached from the world object: `world->vtbl[0x2c]()` (strings, get/set via
`vtbl[0xb0]`/`vtbl[0x1c]`) and `world->vtbl[0x30]()` (objects, set via
`vtbl[0x54]`), holding names such as `strPreScene`, `lProfileManager`,
`lCurrentProfile`.

The world object is `*(interface + 0x59)` - an odd but verified offset
(`mov ecx, [edi+0x59]` in the dispatcher's prologue).

## Dead ends - do not revisit

- **Console `startscene <name>`.** It "succeeds" (`-> 1`) and loads nothing. The
  dispatcher hard-codes `mode 2` and discards the callee's result, so the return
  value never meant anything.
- **`EngineData.dll` RVA `0xEF60` as a level loader.** It is the settings/clip
  entry. `mode 0` returns a pointer, `mode 1` is settings, `mode 2` returns 1 and
  loads nothing. Hitman.ini's `Anisotropy`/`ShadowDetail`/`TextureResolution`
  lines are its `mode=1` calls.
- **`objectDrawA`** (RenderOpenGL RVA `0x6590`) stays `0` while a level renders
  at 62 FPS. It is **not** a world-geometry signal; that part of the renderer map
  needs redoing.
- **Synthesised keyboard input.** `Keys.ps1` now emits hardware scan codes and
  sends key-down and key-up in separate calls with a gap (DirectInput-era games
  ignore VK-only zero-duration taps), and focus was verified. It still never
  skipped the intro. The user presses ESC. Do not pour more time into input
  injection unless it becomes necessary.
- **`llLevels`** is the archive name inside `AllLevels/...` paths, not a
  level-selection property.

## Tooling

```powershell
./tools/fingerprint.ps1          # verify the 11 pins
./tools/build.ps1                # x86 build (Ninja + vswhere-selected MSVC)
./tools/install-mod.ps1          # proxy + manifest (reversible)
./tools/uninstall-mod.ps1        # restore + re-verify pins
./tools/iterate.ps1              # build, install, launch, watch, optional -Scene
./tools/enter-level.ps1          # deploy + launch (+ optional scene request)
./tools/drive.ps1                # write h47vr-go.txt now, no relaunch
./tools/send-keys.ps1            # vk:/text: sequences, scan codes
./tools/steamvr-headless.ps1     # -Action Enable|Disable|Status (null HMD)
./tools/ghidra-analyze.ps1       # headless analysis + anchor extraction
python tools/peutils.py <dll>    # exports with RVAs
```

Ghidra scripts in `tools/ghidra/`: `ExtractAnchors.java`, `DumpReferences.java`
(GL import call sites), `DecompileXrefs.java` (find + decompile whatever
references a string), `DecompileCallers.java`, `DumpRefsTo.java` (refs to an
address), `DecompileAt.java` (materialises a function Ghidra never defined, then
decompiles it - this is what cracked the level start). Run:

```
analyzeHeadless ghidra_project h47vr -process <module> -noanalysis
  -scriptPath tools/ghidra -postScript DecompileAt.java tools/_ghidra_out <abs addr>
```

Vendored: `third_party/openxr` (loader 1.1.65, x86), `third_party/minhook`
(v1.3.4).

## Rules that save time

- Anything touching `E:\`, `%APPDATA%`, or `.git` needs
  `sandbox_permissions: require_escalated`.
- **Deploy before launching.** Steam runs whatever is in the game directory; a
  bare launch silently runs the previous build (`enter-level.ps1` and
  `iterate.ps1` deploy automatically).
- The game must be closed before replacing its DLLs.
- Never call `OutputDebugString` from a detour without a `LoggerActive()` guard -
  the logger calls that API and its mutex is not recursive (this deadlocked the
  game once).
- Recorded module load/unload events are written from the render thread, never
  logged from the loader-notification thread.
- PowerShell `-File` splits comma lists into separate argv entries; split inside
  the script.
- Screenshots: `CopyFromScreen` on the window rect grabs whatever is on top.
  Ask the user, or bring the game forward first.

## Immediate next steps

1. **Camera control** (closes the Milestone 1 gate, and the foundation for
   Milestone 2). `_ZCAMERA_MouseConCam` is at `0x0264D0` and now fires on demand
   after a `push:`, with a stable first argument. Find what it is called with,
   locate the camera transform it uses, modify it from the mod, and confirm the
   view moves. `_ZCAMERA_MainCameraControl` (`0x023940`) and `_ZCAMERA_BaseCamera`
   (`0x020E80`) are the neighbouring candidates.
2. **Redo the world-geometry part of the renderer map.** `objectDrawA` was wrong;
   find the real per-object draw path so a loaded level can be detected without
   the user's eyes.
3. **Milestone 2.** Eye loop at `RenderOpenGL` `0x0048F0` (once per frame);
   recover the `glFrustum` caller for asymmetric per-eye projections (Ghidra has
   not defined the containing function - use `DecompileAt`).
4. Install x32dbg if a calling convention ever needs confirming instead of
   deriving (so far deriving from the machine code has been enough).
