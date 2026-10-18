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
module (cutscenes are `CutSequence` data), so a test loop should push, wait for
the cutscene, then drive the camera.

**The intro and the level openings are in-engine rendered scenes, not video.**
The user confirmed this, and it explains two things that looked unrelated: an
injected view rotation visibly wrecks the openings, and the engine can sit in one
indefinitely (all threads idle, no CPU) until ESC advances it.

**The game pauses when its window is not active - and that was every "stall".**
The user reported it directly: audio keeps playing, nothing draws, and it comes
back when the window is activated. All the launches that went quiet did so
because the tooling shell took focus. It was never a deadlock, a crash, a camera
problem or a video problem. `src/core/src/window_keepalive.cpp` subclasses the
engine's window and swallows
`WM_ACTIVATE`/`WM_ACTIVATEAPP`/`WM_NCACTIVATE`/`WM_KILLFOCUS`, so the engine
believes it is still active and keeps rendering in the background. Verified: 600
frames drawn while another window held focus, then a level push that ran 65 s
untouched. If the game ever behaves strangely around focus or the mouse, this is
the first thing to turn off.

**The camera is now driven from the mod.** The engine transforms its own
geometry, so the view cannot be moved through the GL matrix pipeline; it is
moved where the vertices are, at the proxy's `glVertex3fv`/`glVertex3f` slot.
`h47vr-cam.txt` carries `yaw=.. pitch=..` and about 2,900 eye-space vertices per
frame are rotated by it. Verified numerically; visual confirmation in a loaded
level is the one thing still outstanding.

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

Control files next to the game (all optional, delete to disable):

| File | Effect |
| --- | --- |
| `h47vr-cam.txt` | `yaw=<degrees> pitch=<degrees>` - the injected camera rotation |
| `h47vr-glprobe.txt` | comma list of GL exports to intercept, or `all` / `none` |
| `h47vr-no-glprobe.txt` | shorthand for `none` |
| `h47vr-glmatrix.txt` | while the file exists, log every changed `glLoadMatrixf` matrix |
| `h47vr-stack.txt` | dump a few stacks at `glVertex3fv`, to re-find the engine's transform |
| `h47vr-camobj.txt` | dump whatever the `0x93C50` hook has captured (periodic) |

Two things the injected rotation cannot do, both worth remembering:

- **It cannot defeat the engine's culling.** The engine culls against the
  frustum of the camera it believes it has, so a large injected yaw shows the
  scene with pieces missing. Small offsets - what a head pose actually is - are
  fine; free look needs the engine's own camera driven instead.
- **`yaw` and `pitch` compose as `Rx(pitch) * Ry(yaw)`**, built from explicit
  rotation matrices in row-major storage and applied row by row. The first
  version stored a column-major rotation but applied it row by row, which stays
  orthonormal - so it never looks broken, only subtly wrong.

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
- **GL pipeline intercepted at the proxy boundary.** Every generated proxy thunk
  loads a slot and jumps to it, so writing that slot intercepts the call with no
  code patching and no engine address. `src/proxy_opengl32/gl_probe.cpp` does
  this for the matrix, transform and vertex entries, and publishes an install /
  report pair through `h47vr_core` so the injected-module build stays free of a
  link dependency on the proxy.
- **Camera injection.** A 3x3 rotation is applied to every vertex submitted while
  the current projection is the perspective one; the front end and HUD draw
  through an orthographic projection and are left untouched. About 2,900
  vertices per frame, ~4.2M transformed in one run, and it turns on and off from
  a file with no relaunch.
- **Background rendering.** The engine's window is subclassed and its
  deactivation messages are swallowed, so the game keeps drawing while the
  desktop is used for something else. Without this, every automated run stops the
  moment the shell takes focus, which is what made the project look like it had a
  deadlock for a long time.
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

Per-frame GL traffic in a loaded level (2,400 frames, once the GL probe is in),
which is the shape everything in Milestone 2 has to fit into:

| Entry | Per frame | Notes |
| --- | --- | --- |
| `glVertex3fv` | ~2,900 | eye-space vertices, immediate mode |
| `glBegin` / `glEnd` | ~900 | one pair per primitive batch |
| `glMatrixMode` | ~11 | projection/modelview/texture switches |
| `glViewport` | ~6.7 | |
| `glLoadIdentity` | ~6.8 | |
| `glFrustum` | ~1.9 | 34.52 deg fov, near 5, far 16000 |
| `glOrtho` | ~1.0 | the 2D passes |
| `glLoadMatrixf` | ~2.9 | 1 modelview (identity), 2 others (projection, texture) |
| `glPushMatrix` / `glPopMatrix` / `glMultMatrixf` / `glRotatef` / `glScalef` | 0 | never used |
| `glVertexPointer` / `glDrawElements` / `glDrawArrays` | 0 | no vertex arrays |

`src/core/src/camera_probe.cpp` hooks `0x93C50` and captures its last stack
argument, verifying it points at committed memory before keeping it, and logs
each *distinct* value it sees. That distinct-value log is what disproved the
camera reading above, so it earns its keep as a general way to interrogate a
suspected pointer: if a candidate is really one object it repeats, and if it is
per-object it does not. `h47vr-camobj.txt` dumps the captured object's raw words,
the same words as floats, and the 3x3 behind field `+4`.

**Still missing: the engine's world-to-eye transform.** Established: the
modelview is always identity, the projection is a plain perspective with no
rotation or translation (checked in a loaded level), and `glMultMatrixf`,
`glPushMatrix`, `glPopMatrix`, `glRotatef` and `glScalef` are never called at
all. So the view is applied on the CPU before submission, and the engine culls
first. Without that transform the view cannot be rotated without the culling
cutting the scene away.

Where to look next, from call-validated stacks (see below):

- **`HitmanDlc.dlc` RVA `0x96120`, `FUN_0fd56120`** is the engine frame on both
  the `glVertex3fv` stack (`+0x96884`, sometimes `+0x940BA`) and the `glFrustum`
  stack (`+0x964A3`). It logs `CamUpdate`, lives in `ZCamera.cpp`, and calls both
  `GetGeomGeometry` and the camera's own virtuals. It runs once per geometry
  group - `ECX` is a different object on every call - and its **first stack
  argument is stable across every call** (`03A940E4` in one session), which makes
  that the scene/render object to search for a camera.
- `RenderOpenGL.dll` `+0x008A78` and `+0x0070ED` are the direct callers of
  `glVertex3fv`; `+0x004483` and `+0x0292FA` sit above them in every sample.
  `glFrustum` comes from `RenderOpenGL.dll` `+0x00346F`, `+0x02F340`,
  `+0x028EB9`, `+0x02E2BB`.

The stack scan now rejects any candidate whose preceding bytes do not decode as
a call, which is what makes the chain above worth trusting: the earlier,
unvalidated scan produced a plausible but wrong answer.

**`FUN_0fd53c50` (RVA `0x93C50`) is not the camera, and is not on the world-to-eye
path.** It was misread as one. Live capture of its last argument gives a
different pointer on every call - eight distinct values inside the first
thirteen - and the object behind it holds `[[0,0,1],[0,1,0],[1,0,0]]`, a
coordinate-convention swap rather than a view. The `(float)` casts in its
decompilation are int-to-float on *integer* origins, not float bit patterns. It
is the per-object quantized-vertex path: subtract an integer origin, rotate into
the object's frame, `__ftol`, pack.

The RVAs from that stack scan (`HitmanDlc.dlc` `+0x93C16`, `+0x93EA0`,
`+0x95EE5`, `+0x96884`, `+0x1BB873`; `RenderOpenGL.dll` `+0x008A78`,
`+0x004483`, `+0x0289F5`, `+0x02902B`, `+0x02E2E6`, `+0x02F300`) are therefore
hints at best - a crude stack scan finds stale code addresses just as readily as
return addresses. `camera_probe.cpp` remains useful, but as a pointer-capture
mechanism, not as a camera finder.

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
- **Moving the camera through the GL matrix pipeline.** There is no camera
  matrix there to move: `glLoadMatrixf` in modelview is identity once per frame,
  and `glPushMatrix`, `glPopMatrix`, `glMultMatrixf`, `glRotatef` and `glScalef`
  are never called at all across a whole session (0 calls over 2,400 frames).
  The only matrix in the pipeline is the projection, and it is camera-independent.
- **`_ZCAMERA_*` exports as the per-frame camera.** They are wing-group class
  *descriptors*. `_ZCAMERA_MouseConCam` fills a descriptor with a constructor, a
  float, an id and its name; the object is built later by that constructor
  (0x93 bytes, vtable `PTR_FUN_0feb1748`; `BaseCamera` 0x32 bytes, vtable
  `PTR_FUN_0feb1104`). Hooking the export reports class instantiation, not camera
  updates.
- **Screen capture and synthetic input from the tooling shell.** `PrintWindow`
  returns a black frame and both `CopyFromScreen` and a raw `BitBlt` from the
  desktop DC fail with "the handle is invalid" even when escalated, while window
  enumeration by PID works fine. Verification has to be numeric, or come from the
  user.
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
- Screenshots from the tooling shell do not work at all here (`PrintWindow`
  returns black, `CopyFromScreen` and `BitBlt` fail) - see the dead ends. Do not
  spend a launch on a picture; make the check numeric, or ask the user.
- The proxy's matrix log is gated on `<game>\h47vr-glmatrix.txt`: the front end
  rewrites its projection and texture matrices every frame and would spend the
  whole budget before a level loads.
- The GL probe can be bisected without a rebuild: put the export names in
  `h47vr-glprobe.txt`, or `none` to turn the whole probe off.

## Immediate next steps

1. **Close the Milestone 1 gate**: confirm the view really moves in a loaded
   level. The user's eyes are the only reliable observer here. `h47vr-cam.txt`
   with `yaw=60` is on, and the injected rotation is the only change to the
   picture. After that, turn the fixed offset into a pose: the same vertex hook
   is where per-eye rotation and translation belong.
2. **Make the camera two-eyed.** The projection is set by `glFrustum` (34.52 deg
   fov, near 5, far 16000) and by hand into a 45 deg matrix; both entries are
   already intercepted, so stereo is a change of arguments and a second viewport
   rather than a new hook.
3. **Redo the world-geometry part of the renderer map.** `objectDrawA`
   (RenderOpenGL RVA `0x6590`) never runs - `glPushMatrix`, `glPopMatrix` and
   `glMultMatrixf` are 0 calls - so the world path is immediate mode:
   `glBegin`/`glEnd` with ~2,900 `glVertex3fv` per frame. "The perspective
   projection is current and vertices are flowing" is a usable level signal that
   needs no eyes.
4. **Milestone 2.** Eye loop at `RenderOpenGL` `0x0048F0` (once per frame).
5. Install x32dbg if a calling convention ever needs confirming instead of
   deriving (so far deriving from the machine code has been enough).
