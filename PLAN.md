# Hitman: Codename 47 VR Mod - Feasibility and Development Plan

Document status: agreed plan of record, 2026-10-18. This is the merged plan and
supersedes the chat-transcript copy saved in this repository as `PLAN.MD.txt`.

## Progress Checklist

This section is the single source of truth for where the project stands. Update
it in the same commit as the work it describes, so the checkbox list never lags
the code.

Legend: `[x]` done and verified, `[~]` partial or in progress, `[ ]` not started.

### Foundation and tooling

- [x] Supported-build fingerprints, shared by the PowerShell tooling and the compiled mod
- [x] App-local `opengl32.dll` proxy, loading in-process ahead of the system library
- [x] Full 368-export shadow surface (required: `gdi32` calls `opengl32!wgl*` at run time)
- [x] Fingerprint gate that refuses to patch an unsupported build or unknown revision
- [x] Diagnostic logging under `<game>\h47vr-logs`, flushed per line
- [x] Install and uninstall that preserve the commercial copy, verified by re-running the pins
- [x] One-command development iteration (`tools/iterate.ps1`)
- [x] Standalone injectable module (`h47vr_mod.dll`) plus launcher `verify` / `run` / `inject`
- [x] Ghidra headless pipeline plus shared PE tooling (`tools/ghidra-analyze.ps1`, `tools/peutils.py`)
- [x] Hook targets resolved by exported name, confirmed live for `Globals.dll` and
      `System.dll` (`g_pSysInput` at RVA `0x3014`, `RunEngine` at `0xA790`)
- [x] Deferred hook installation on module load (`LdrRegisterDllNotification`); fires when
      `HitmanDlc.dlc` arrives, which is after the proxy starts
- [x] Console/config command dispatcher located (`System.dll` RVA `0xE590`) with its ABI read
      from the machine code (`__thiscall`, two stack arguments, `ret 8`), not from the
      decompiler; `self` is the `Globals.dll` interface singleton at RVA `0x3010`
- [~] In-process console driver: submits `startscene <scene>` to that dispatcher from the
      presentation hook, so a level loads without synthesising keystrokes (built; one run
      pending)
- [x] First real detours in the shipping build: `gdi32!SwapBuffers` (firing, frames counted)
      and `_ZCAMERA_MouseConCam` (installed at RVA `0x0264D0`)
- [x] Renderer map: GL import call-site analysis (`tools/ghidra/DumpReferences.java`) plus
      in-process call-rate confirmation, which identified the frame boundary and the four
      2D UI passes
- [ ] Controllable camera: drive the camera from the mod and observe it move in a loaded level
- [ ] Reach gameplay automatically: `objectDrawA > 0` after the console driver submits
      `startscene C0_1` (blocker for the Milestone 1 gate)
- [x] Vendor the 32-bit `openxr_loader` (`third_party/openxr`, built from
      `release-1.1.65` by `tools/fetch-openxr.ps1`; verified i386 with the expected exports)
- [ ] Install x32dbg for live address and calling-convention validation
- [ ] Vendor MinHook

### Milestone 1 - baseline and integration points

- [x] Fingerprint the installed modules and record existing plugins and configuration
- [x] Work in a separate development copy, preserving the Steam installation
- [x] Verify normal gameplay in both renderers (Direct3D and OpenGL both run)
- [~] Isolate the widescreen plugin's effects (loaded and pinned; behaviour unmeasured)
- [~] Locate camera transforms, world rendering, visibility calculation, input dispatch and weapon firing
      (camera, visibility, input and weapon located by exported name; renderer draw candidates
      identified by RVA; no semantics confirmed in a debugger yet)
- [x] Graphics bridge gate: prove one 32-bit process can hold the legacy WGL context and an
      OpenXR session at once. Passed headless via SteamVR's null driver - instance created,
      system found, 2 eyes at 1512x1680, GL 4.6 context, session READY, `xrBeginSession` ok
- [~] Validate the same through Virtual Desktop on the Quest (VDXR creates instances fine and
      advertises `XR_KHR_opengl_enable`; `xrGetSystem` needs an active stream session, so this
      awaits a connected headset)
- [~] Deliver a hook map and a graphics-bridge result (hook map now carries resolved symbols and
      RVAs; graphics bridge still untested)

Gate: the VR graphics path is now proven. The controllable camera still is not,
so interaction work stays blocked until a camera hook exists.

### Milestone 2 - correct stereo rendering

- [~] Update gameplay once, then render both eyes from the same simulation state
      (frame boundary identified: `RenderOpenGL.dll` RVA `0x0048F0`, the only caller of
      `SwapBuffers`; measured at ~1 call per frame. `FUN_0fb73680` is per-pass at ~17-18
      calls per frame, so it is the wrong place for the eye loop)
- [~] Use the runtime's eye poses and asymmetric projections (runtime side proven by the
      probe; the renderer side is blocked: `glFrustum` is called from a code region Ghidra
      has not defined as a function, and recovering it is the next step)
- [ ] Apply camera changes before visibility calculation; expand visibility conservatively
- [ ] Prevent the second eye pass from repeating animation, scripts, audio, particles or weapon events
- [ ] Calibrate game units to metres, eye height and near clipping
- [ ] Hide the player's head and separate gameplay camera control from headset orientation

Gate: positional parallax, stable world scale, unchanged simulation speed in training.

### Milestone 3 - playable VR slice

- [ ] Tracked controller hands and independent weapon aiming
- [ ] Shots follow the weapon muzzle direction, with nearby obstruction checks
- [ ] Trigger firing, grip-based grabbing, two-hand stabilisation, basic haptics
- [ ] Radial equipment menu; buttons retained for reloads, disguises and scripted actions
- [ ] Interactions routed through original gameplay rules
- [ ] Stick movement, snap turning, recentering, configurable handedness
- [ ] Physical head movement near walls handled with collision limits and a fade

### Milestone 4 - training and the first Hong Kong mission

- [ ] Menus, map, inventory and briefings on a controller-operated VR panel
- [ ] Cutscenes on a virtual screen
- [ ] Scopes through a dedicated aiming view
- [ ] Verify loading, death, restarts, scripted camera changes and mission completion
- [ ] Package plugin, launcher, configuration, diagnostic logging and removal instructions

### Later, explicitly out of scope for the slice

- [ ] Manual magazine handling
- [ ] Physics-based melee
- [ ] Physical garrote simulation
- [ ] Campaign-wide certification

### Constraints discovered, to keep respecting

- [x] Launch chain characterised: the game uses legacy `steam.dll` and only accepts a
      launch performed by the Steam client, so testing happens in the game directory
- [x] Presentation identified: `SwapBuffers` comes from `gdi32`, not `opengl32`
- [ ] Widescreen plugin's effect on the OpenGL path measured
- [x] Ghidra pass over the binaries (`HitmanDlc.dlc`, `System.dll`, `Globals.dll`,
      `EngineData.dll`, `RenderOpenGL.dll` analysed; 190 anchors and 1927 references
      in the gameplay module)

## Recommendation and Feasibility

Build a native 32-bit x86 mod that hooks the commercial Steam copy of the game,
guided by targeted reverse engineering. A complete decompilation is unnecessary
and stays off the critical path.

Target: Quest through PC VR (Virtual Desktop), tracked controllers, hybrid
physical interaction, initially covering the training level plus the first Hong
Kong mission.

| Capability | Assessment |
| --- | --- |
| Runs alongside the commercial copy | Strong. App-local proxy DLL loading is already proven on this install by the third-party `DSOUND.dll` wrapper |
| First-person stereo and head tracking | Plausible. Needs a rendering prototype to confirm |
| Controller-directed weapon aiming | Plausible. Needs gameplay hooks in `Hitmandlc.dlc` |
| Tracked hands, grabbing, two-hand support | Substantial custom work |
| Manual reloads, physics melee, garrote simulation | Separate, much larger extension. Explicitly post-slice |
| Campaign-wide compatibility | Requires extensive mission testing |

These are engineering judgements, not demonstrated results. The installed files,
imports, exports and configuration were inspected; the game has not been run or
instrumented yet.

### Verified Environment Facts

- All involved modules are 32-bit x86. `Hitman.Exe` is a thin host of about
  20 KB that imports only kernel32. `Hitmandlc.dlc` is a 2.5 MB Windows module
  holding roughly 5,835 functions of gameplay code despite its extension.
  `System.dll` exports `RunEngine` and `EditPrint`. `Globals.dll` exports the
  engine singletons, including `g_pSysInput`.
- `ar405.exe` is not the game. It is a bundled Adobe Acrobat Reader 4.05, used
  for the manual PDF.
- `RenderOpenGL.dll` and `RenderD3D.dll` each export exactly one symbol,
  `Create`. The renderer is a swap-in module selected by `DrawDll` in
  `Hitman.ini`. The installed configuration currently selects Direct3D
  (`DrawDll RenderD3D.dll`), so selecting OpenGL is a deliberate step.
- `RenderOpenGL.dll` is fixed-function OpenGL 1.1: `glFrustum`, `glMultMatrixf`,
  `glTranslatef`, `glViewport`, `glPushMatrix`, client-side vertex arrays drawn
  with `glDrawElements`, textures through `glTexImage2D`/`glTexSubImage2D`, and
  presentation through `SwapBuffers`. No shaders, VBOs or FBOs. Command capture
  and replay is therefore cheap, and only our own eye-render context needs
  modern OpenGL.
- Input is Win32-based and driven by the SCode action map in `Hitman.cfg`, for
  example SCode 511 mapped to both `TurnLeft` and `TurnRight`. No module imports
  DirectInput.
- Virtual Desktop's OpenXR runtime is registered for both process
  architectures, including `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime`
  pointing at `virtualdesktop-openxr-32.json`. SteamVR also ships a genuine
  32-bit OpenXR runtime (`bin\vrclient.dll`) plus a null HMD driver; the
  original note here claimed SteamVR was 64-bit only and was corrected during
  Milestone 1. The Oculus/Meta runtime entries on this machine are stale - the
  manifests they point at do not exist.
- An existing third-party `DSOUND.dll` proxy wrapper and the `h1w.asi`
  widescreen fix are installed. Their project names are leaked in debug strings.
  Neither should be hooked or replaced, and both need compatibility testing.
- Embedded `Z:\` build paths leak the original source layout: 146 unique paths
  in the gameplay module alone, plus named camera classes and RTTI class-info
  objects. See Reverse Engineering Anchors.

### Supported Build Pins

The mod must refuse to patch any other build. Steam appid 6900, buildid 251800.

| File | SHA-256 |
| --- | --- |
| `Hitman.Exe` | `137C3CBF328225085CF3532819B4574714FBA263A6121328DE6583BA8A0648F5` |
| `Hitmandlc.dlc` | `201AF466CB75048314D38572446505DFF54B233D2060E678BF76DA871E1CD7C1` |
| `System.dll` | `DB2A6AC1664E04AE6C36ED70321A07279196453A1458F0283DC5D17D2B73BC95` |
| `RenderOpenGL.dll` | `3C15227D21760590947E7023C721D6290CCEC04E67138AA46ED47BC8A9C4663A` |
| `EngineData.dll` | `01A20275C75C960EBCD3D13A81D9BDEF051FA9AD3D3B1DC22FBF6BB21D5EFEDA` |
| `Globals.dll` | `010E8F9B8596E33EECF41CB303DFC12A702F9ECB600C5890AC66317C6E3AD750` |
| `gsc.dll` | `663CA7A8DAAE84406A3B2A8344B3C509D9859BC1D9E180ED6632BF48338EF8EF` |
| `Locale.dll` | `6391BE08D43C12D91E20AF4829C3420A269292462B7EBBCF3BB64C966989AA33` |
| `Sound.dll` | `1CA948CFC078819346E8FC2419F39FEA5DE3C8CE012D00DC771BC5F17672E794` |
| `DSOUND.dll` | `FF66B6CF72CD8C58F09016B4E2428B4BF0984CCCDBC2781C0EB94DA2966DE8E4` |
| `scripts\h1w.asi` | `8E5D4E82E603B51C6BADDAF95EC31CA7ABDD1B7776B0A5963FC5B66DCBDA2702` |

## How the Decompilation Projects Help

**re47: useful reference, insufficient as a replacement engine.** Its published
status lists the main gameplay module and the OpenGL renderer at 0% decompiled,
with partial function naming, and Direct3D untouched. It uses the GOG build, so
its addresses cannot be assumed to match this Steam installation, and the
repository is archived.

Its build configuration includes an experimental mod framework. The free-camera
source hooks a projection-related function and rewrites OpenGL transforms using
a fixed address. That is a useful clue but demonstrates neither stereo rendering
nor working first-person play nor independent aiming. Reidentify the function in
the Steam binaries and validate its behaviour before relying on anything from
it.

**decompdecomp: optional research aid only.** It offers function matching with
Ghidra assistance, but Windows support is beta and its platform table does not
promise a rebuildable Windows export. Matching the original compiler's output is
a different goal from implementing VR. Keep it off the critical path and
consider it only for individual hard functions, after validating its workflow
against this game.

**Ghidra is the main static-analysis tool.** Recover only the camera, rendering,
input, weapon, collision and interaction interfaces the mod needs, and confirm
decompiler interpretations in a debugger.

Keep original game binaries and assets out of the mod package. Treat re47 as
reference material and do not incorporate its code.

### Reverse Engineering Anchors

The binaries leak `Z:\` source paths and RTTI names, which gives a subsystem map
before any decompilation.

- Camera: `Z:\Hitman\Camera\Source\MouseConCam.cpp`, `_ZCAMERA_MouseConCam`,
  `_ZCAMERA_MainCameraControl`, `_ZCAMERA_BaseCamera`, `_ZCAMERA_NewFollowCam2`,
  plus `CSniperCamera` and `CBinocularCamera`. The exported
  `?ZCAMERA_ClassInfo@@3VZCLASSINFOZCAMERA@@A` object means the active camera can
  be located by class name at runtime rather than through a frozen address.
- Weapon: `_ZITEM_FireShot`, `ZItemGroupWeapon`, `ZItemAmmo`.
- Interaction: `Z:\Hitman\Bkgnd\Source\ZItemActivate.cpp`, with a
  `BasePickupTarget` routine and pickup-target error strings. Interactions must
  route through this path so distance checks, inventory, AI reactions and
  mission scripts stay authoritative.
- Other confirmed subsystem directories: `Z:\Hitman\Laptop\Source`,
  `Z:\Hitman\CUI\Source`, `Z:\Hitman\Enemies\Source`, `Z:\Engine\EventsBase\Source`,
  `Z:\Engine\GeomsBase\Source`, `Z:\Engine\GeomsExtend\Source`,
  `Z:\Engine\Dynamics\Source`, `Z:\Engine\ZStdLib\Source`.

## Architecture and Tools

A modern C++ x86 plugin with these internal boundaries:

- **Game adapter:** validated hooks for camera, player movement, weapon firing,
  interaction targets and game state.
- **VR backend:** OpenXR session, predicted headset and controller poses,
  actions, haptics and frame submission.
- **Rendering adapter:** eye rendering, projection overrides, render targets and
  separation of world geometry from menus.
- **Interaction layer:** tracked hands, weapon alignment, two-hand support,
  grabbing and existing game actions.

Loading: an app-local `opengl32.dll` proxy that forwards to the system library by
absolute path. This keeps the game launchable from Steam and creates no
dependency on the unidentified loader, which is now identified as a third-party
`DSOUND.dll` wrapper that we neither hook nor replace. A small launcher stays
available as a development and diagnostic path. Hooks resolve from module
fingerprints plus validated signatures, and unsupported builds are refused
before any patch is applied.

| Tool | Planned purpose |
| --- | --- |
| Ghidra with a JDK (not yet installed) | Structures, calling conventions, call paths |
| x32dbg from x64dbg (not yet installed) | Live address, argument and state validation |
| MinHook | Install x86 function hooks |
| apitrace | Inspect OpenGL output and separate world from UI passes |
| OpenXR SDK (x86 loader must be vendored, none on this machine) | Runtime validation and VR integration |
| MSVC 14.44 x86 through VS2022 and CMake (installed), mingw i686 as fallback | Build the mod independently of the original compiler |

Start with the OpenGL renderer, which exposes the useful matrix operations, and
keep Direct3D as a fallback investigation route rather than implementing two
backends.

The first technical gate is the graphics bridge: prove direct OpenXR/OpenGL
compatibility, including the runtime's context requirements, on the connected
Quest. If that is unavailable, test OpenGL-to-D3D11 GPU texture sharing and
submit through OpenXR/D3D11. Do not assume that registered 32-bit runtime
support guarantees either path. CPU image copies are acceptable for diagnostics,
not for the release architecture.

## Development Milestones

**1. Baseline and integration points.** Work in a separate development copy,
preserving the Steam installation. Fingerprint the installed modules and record
existing plugins and configuration. Verify normal gameplay in both renderers and
isolate the widescreen plugin's effects, then switch deliberately to OpenGL.
Locate camera transforms, world rendering, visibility calculation, input
dispatch and weapon firing. Validate an x86 OpenXR session on the connected
Quest. Deliver a hook map and a graphics-bridge result.

Gate: proceed only after a workable VR graphics path and a controllable camera
exist. Otherwise document the failed assumptions and revise the architecture
before building interactions.

**2. Correct stereo rendering.** Update gameplay once, then render both eyes from
the same simulation state, using runtime eye poses and asymmetric projections.
Apply camera changes before the relevant visibility calculations and expand
visibility conservatively where needed. Prevent the second eye pass from
repeating animation updates, scripts, audio, particles or weapon events.
Calibrate game units to meters, eye height and near clipping. Hide the player's
head where needed and separate gameplay camera control from headset orientation.

Gate: demonstrated positional parallax, stable world scale and unchanged
simulation speed in the training level.

**3. Playable VR slice.** Tracked controller hands and independent weapon
aiming. Shots follow the actual weapon and muzzle direction, with nearby
obstruction checks. Trigger firing, grip-based grabbing, two-hand stabilisation
and basic haptics. A radial equipment menu, with buttons retained for reloads,
disguises and complex scripted actions. Interactions route through original
gameplay rules. Stick movement, snap turning, recentering and configurable
handedness. Physical head movement near walls handled with collision limits and
a fade.

**4. Training and the first Hong Kong mission.** Use Hong Kong to exercise
outdoor visibility, distant targets, scopes, NPC behaviour and mission
transitions. Menus, map, inventory and briefings on a controller-operated VR
panel. Cutscenes on a virtual screen initially, scopes through a dedicated
aiming view. Verify loading, death, restarts, scripted camera changes and
mission completion. Package the plugin, configuration, diagnostic logging and
removal instructions.

Manual magazine handling, physics-based melee, physical garrote simulation and
campaign-wide certification remain later milestones.

## Acceptance Tests and Effort Assumptions

Release the slice only when all of the following hold:

- Stereo remains correct across head translation, rotation, recentering and
  turning.
- Rendering twice does not double gameplay events or change game speed.
- Controller aim, muzzle position, impact location and obstruction handling
  agree.
- Grabbing and two-hand support preserve original inventory and mission
  behaviour.
- Menus, scopes, death and scripted sequences remain usable.
- Lost tracking, headset removal and runtime focus changes recover cleanly.
- The training level and the selected mission can be completed entirely in VR.
- Installation and removal preserve the commercial copy, and unknown builds
  receive a clear compatibility message.

Target native 72 Hz initially, a 13.9 ms frame interval, then assess higher
refresh rates from measured CPU and GPU timings. Runtime and encoding overhead
require additional headroom, and the engine is documented to misbehave at high
frame rates with camera shake and floating items, so the engine tick is capped
independently of the render rate.

Assume controller tracking, with Virtual Desktop as the first runtime to
validate. Exact Quest model and GPU remain hardware-validation inputs.
Standalone Quest execution is outside this plan.

### Open Risks

- Graphics bridge: **retired**. Direct OpenXR/OpenGL coexistence in one 32-bit
  process is proven (headless, via SteamVR's null driver). The D3D11 sharing
  path is no longer needed.
- Capture and replay fidelity: if the engine's draw stream cannot be replayed
  cleanly, the fallback is hooking and re-entering the scene-draw function.
- Independent weapon aiming and interaction hooks may not exist cleanly. The
  documented fallback is head-aim with a hand-mounted weapon model.
- 32-bit OpenXR limits v1 to runtimes that ship an x86 runtime. Virtual Desktop
  and SteamVR both do. This risk is retired: the graphics bridge passed headless
  through SteamVR's 32-bit runtime.

For one experienced developer, budget approximately 2 to 4 weeks for the
feasibility investigation, followed by several months for a polished playable
slice. These are provisional estimates. Stereo rendering and independent weapon
control must be proven before any delivery schedule is credible.
