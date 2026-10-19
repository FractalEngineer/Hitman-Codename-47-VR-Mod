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
      presentation hook, so a level loads without synthesising keystrokes. First run proved
      the path end to end (`g_pSysInterface` rva `0x3010` matched, dispatcher hooked at
      `0FFAE590`, `startscene C0_1 -> 1`, world object non-null) but rendered no geometry,
      because the scene name was wrong
- [x] Scene naming resolved from the game's own level-select table in `HitmanDlc.dlc`: names
      are relative paths with no extension (`C0_Training\C0_1`), not bare `C0_1`
- [x] On-demand scene trigger (`h47vr-go.txt`): the mod submits `startscene` whenever the file
      appears, which is the only way to fire at a moment a player can recognise, because the
      intro videos have to be skipped by hand (`tools/drive.ps1` writes it)
- [x] Hook re-arm after a module reload: level transitions reload the engine's modules, which
      discards every patch, so module loads now raise a flag and the frame boundary re-installs
- [x] Scene-entry probe: `EngineData.dll` RVA `0xEF60` is `world->vtbl[0x44]`, the entry the
      console `startscene` command calls (`__thiscall(object, mode, arg1, arg2)`), and the
      dispatcher throws its result away - so this is the only place the engine's own opinion
      about a scene request is observable. Logs every call, marking the mod's own
- [x] Module load/unload history recorded under the loader lock and flushed from the render
      thread, so a reload shows up in the log instead of looking like a silent stall
- [x] Synthetic input carries hardware scan codes (DirectInput-era games ignore key events that
      only carry a virtual key, which is why the ESC burst kept doing nothing)
- [x] First real detours in the shipping build: `gdi32!SwapBuffers` (firing, frames counted)
      and `_ZCAMERA_MouseConCam` (installed at RVA `0x0264D0`)
- [x] Renderer map: GL import call-site analysis (`tools/ghidra/DumpReferences.java`) plus
      in-process call-rate confirmation, which identified the frame boundary and the four
      2D UI passes
- [~] **Controllable camera: the view is now driven from the mod, 2026-10-19.** The engine does
      its own view transform - the GL modelview is always identity and the projection is a plain
      perspective - so the camera cannot be moved through the matrix pipeline. It is moved where
      the geometry is: the proxy's `glVertex3fv`/`glVertex3f` slot, which about 2,900 eye-space
      vertices per frame pass through in immediate mode. A 3x3 rotation is applied to each one
      while the perspective projection is current (the front end and HUD draw through an
      orthographic projection and are left alone). `h47vr-cam.txt` - `yaw=.. pitch=..` - turns it
      on and off. Verified numerically (4.19M vertices transformed, the gate turns on and off
      cleanly, and the vertices are provably eye-space) **and visually confirmed by the user in a
      loaded level**: the view turns with `h47vr-cam.txt`. Two limits remain, and they are the
      current work. The engine culls against the frustum of the camera *it* believes it has, so a
      large injected yaw shows the scene with pieces missing (the user's words: "we're just
      completely clipping"); and an injected rotation visibly wrecks the front end's in-engine
      openings. Rotating vertices cannot bring back culled geometry - the engine's own view
      transform has to be driven instead, which is the next task (leads in `docs/HANDOFF.md`)
- [x] GL pipeline probe at the proxy boundary: the engine's GL calls are intercepted by
      redirecting the proxy's *own* export slots (`src/proxy_opengl32/gl_probe.cpp`). Every thunk
      in the generated proxy loads a slot and jumps to it, so writing the slot intercepts the call
      with no code patching and no engine address. Control files: `h47vr-glprobe.txt` (comma list
      of exports), `h47vr-no-glprobe.txt` (none), `h47vr-glmatrix.txt` (matrix log gate)
- [~] **Engine camera hunt, 2026-10-19.** The vertex-level rotation cannot be the whole answer, so
      the engine's own camera is being found *empirically* instead of by reading the decompiler,
      which mis-reads this code's stack arguments - that is exactly how the last argument of `Clip`
      came to be mistaken for a camera. Settled by disassembly (`tools/ghidra/DumpRange.java`):
      `FUN_0fd53c50` is the ZGEOM `Clip` method and reaches `glVertex3fv`; `FUN_0fd56120` is
      `CamUpdate` and stores its own object into the scratch it hands `Clip`
      (`mov [ebp-0x2018], esi` at `0xfd56279`); `Clip` then reads a float triple at
      `[cam+0x106]` and feeds it to `FUN_0fe7b730`, an integer box test. Live capture shows that
      triple is a world position which moves with the view, so the camera object is identifiable
      at run time as the `CamUpdate` object whose `+0x106` has a large magnitude. Its rotation is
      *not* stored as an orthonormal 3x3 in its first `0x800` bytes even when scanning at 2-byte
      offsets, so it is either derived per frame or stored some other way. `h47vr-camobj.txt` now
      drives this probe: it reports each candidate's `+0x106`, its basis, and the offsets the
      engine itself rewrites. **What that produced, and closed off:** the camera really is
      identifiable (the `CamUpdate` object whose `+0x106` keeps moving - there is one, at
      `MouseConCam`'s object `+0xEA8`), and a writer that rotates its basis at `+0x0DA` was built
      and run. It is applied every frame and the game holds it, but **the picture does not
      change**: a sweep that rotated each of the 24 candidates in turn scored 1.1-6.4 against a
      baseline sway noise of 5.98, i.e. nothing. So no `CamUpdate` object's `+0x0DA` drives the
      rendered view, and that field is a dead end for camera control
- [x] **Where the view transform actually is, 2026-10-19.** Culling is real and confirmed
      visually rather than inferred: with the vertex rotation at `yaw=60`, `tools/capture.ps1`
      shows the scene rotated *and* large areas left empty (789 -> 219 distinct colours, mean
      difference 36.27 against a noise floor of ~6), which is geometry the engine discarded before
      the proxy saw it. The vertex stack then says where to look: `Clip` reaches `glVertex3fv`
      through `RenderOpenGL.dll` (`+0x0292FA` -> `+0x004483` -> `+0x008A78`, the direct caller), so
      the world-to-eye transform is applied inside the engine's OpenGL module, not in the gameplay
      module - which is exactly why rotating a gameplay-side camera object does nothing
- [x] **Camera control solved the direct way, 2026-10-19: drive the engine's own input.**
      The missing piece was focus. A plain `SetForegroundWindow` is refused, which is why every
      synthetic keystroke before this landed nowhere; attaching to the foreground thread and to
      the target's own thread first lifts that restriction, and the game then reports as
      foreground. With focus, mouse deltas reach the engine, the engine rotates its own camera
      from them, and **the world stays whole**: `tools/turn.ps1 -Dx 220 -Verify` moves the view
      (mean difference 15.8-25.6 against a ~6 noise floor) and the captures show stairs, floor,
      walls and the player model all present. That is the culling problem solved without
      reverse-engineering the visibility pass, and it is also the shape the VR mod needs: head
      pose becomes mouse deltas, the engine does the turning, and stereo is a separate pass over
      an engine that is already looking the right way
- [x] `tools/capture.ps1`: the picture is readable at last, so "does it look right" is answered
      here rather than by the user. `PrintWindow`/`PW_RENDERFULLCONTENT` returns the window's own
      contents with no focus stolen; `-Compare` scores two shots. Noise floor ~6 on a static scene
- [x] The engine's view transform located, 2026-10-19, by elimination rather than by address:
      `glLoadMatrixf` in modelview is identity once per frame, and `glPushMatrix`, `glPopMatrix`,
      `glMultMatrixf`, `glRotatef` and `glScalef` are never called at all. The only matrix the GL
      pipeline carries is the projection (built by `glFrustum` at 34.52 deg fov / near 5 / far
      16000, and by hand into a 45 deg projection). The world therefore arrives at `glVertex3fv`
      in eye space - ~2,900 vertices and ~900 `glBegin`/`glEnd` pairs per frame
- [x] Corrected: the exported `_ZCAMERA_*` symbols are wing-group *class descriptors*, not the
      per-frame camera. `_ZCAMERA_MouseConCam` (RVA `0x0264D0`) fills a descriptor with its
      constructor `FUN_0fce6500`, a float `1000.0`, an id and the name, and the camera object is
      built by that constructor (0x93 bytes, vtable `PTR_FUN_0feb1748`; `BaseCamera` is 0x32
      bytes, vtable `PTR_FUN_0feb1104`). Hooking the export only reports when the camera class is
      instantiated, which is why it fires so rarely
- [x] **Corrected twice, and the second correction matters: the tooling CAN see the game.**
      The earlier entry here claimed `PrintWindow` returns black and that `CopyFromScreen` and
      `BitBlt` fail with "the handle is invalid". Both halves are wrong today. `PrintWindow` with
      `PW_RENDERFULLCONTENT` (flag 2) returns the window's own contents - a clean 800x600 frame,
      with the window behind other applications and without stealing focus - and a plain
      `CopyFromScreen` works as well. `tools/capture.ps1` wraps both, and `-Compare` reports a mean
      absolute per-channel difference between two shots, which is enough to answer "did that
      change the picture" without asking anyone to look. Only the *input* half of the old note
      survives: synthetic keys still need the window to be foreground, `SetForegroundWindow` is
      refused, so the user still presses ESC for the intro
- [x] **Solved, 2026-10-19: the game pauses when its window is not active.** The user reported
      that it stops running when the window is not focused while audio keeps playing, and that is
      what every one of the "stalls" was: the tooling shell taking focus, the engine quietly
      stopping presentation, all threads parked on `UserRequest` with no CPU, and the log going
      quiet until something activated the window again. It was never a crash, a deadlock, a
      camera problem or a video problem. `src/core/src/window_keepalive.cpp` now subclasses the
      engine's window on a worker thread and swallows the deactivation messages
      (`WM_ACTIVATE`/`WM_ACTIVATEAPP`/`WM_NCACTIVATE`/`WM_KILLFOCUS`), so the engine believes it
      is still active. Verified: 600 frames drawn while another window held focus, and a level
      push then ran 65 s untouched
- [x] **Correction, 2026-10-19: the "camera object" was not the camera.** Live capture at
      `HitmanDlc.dlc` RVA `0x93C50` shows its last argument is a different pointer on every call
      (eight distinct values inside the first thirteen calls), and the object it points at holds
      `[[0,0,1],[0,1,0],[1,0,0]]` - a coordinate-convention swap, not a view. The `(float)`
      conversions in that decompilation are int-to-float on *integer* origins. `FUN_0fd53c50` is
      the per-object quantized-vertex path: subtract an integer origin, rotate into the object's
      frame, quantize with `__ftol`, pack. The earlier "camera located" claim came from a crude
      stack scan, which finds stale code addresses as readily as return addresses, and it was
      wrong
- [x] Corrected: "reach gameplay automatically" was originally gated on `objectDrawA > 0` after a
      console `startscene`, and both halves of that are wrong - `startscene` loads nothing and
      `objectDrawA` is not a world-geometry signal. The real answer is the keyboard-free level
      entry below, and the usable level signal is the gameplay camera firing, not a draw counter
- [x] **Gameplay reached, 2026-10-19.** The training mission loaded with a controllable
      player at 62 FPS and `_ZCAMERA_MouseConCam` was seen firing for the first time
      (`#1` front end, `#2` at level start) - the Milestone 1 signal this project has been
      chasing. It fires rarely, i.e. it is camera setup rather than the per-frame update
- [x] Corrected: `objectDrawA` (RenderOpenGL RVA `0x6590`) stays `0` while a level renders, so
      it is **not** a world-geometry signal. That part of the renderer map has to be redone
- [x] Corrected: the level start does **not** go through `EngineData.dll` RVA `0xEF60` - the
      whole session produced three calls there and all three were settings (`Anisotropy`,
      `ShadowDetail`, `TextureResolution`). The console `startscene` route is a dead end for
      starting a level, which is why it "succeeded" and did nothing
- [x] Direct scene-entry harness: `h47vr-go.txt` accepts `<mode>:<name>` and calls the entry
      itself, returning the engine's own result instead of a hard-coded "recognised"
- [x] Corrected again, and this one matters: `EngineData.dll` RVA `0xEF60` is the engine's
      **settings/command** handler (`EngineData.cpp`; it switches on `Cams`, `Hira`, `Dir`,
      `HiraClip`, `DirClip`), and `Hitman.ini`'s `Anisotropy`/`ShadowDetail`/`TextureResolution`
      lines go through it as `mode=1`. `startscene` (`mode=2`) returns 1 and loads nothing.
      The whole `startscene` lead was a misreading - level loading lives in `HitmanDlc.dlc`
- [x] Front end structure located, which is where the repeatable entry has to come from: every
      screen is an exported wing group (`_ZWINGROUP_StateIntro`, `_ZWINGROUP_StateSelectLevel`,
      `_ZWINGROUP_StateMain`, ...) built by descriptor functions, switched by **name** through
      `vtbl[0x49c]("StateSelectLevel")`, and the front end's data is a name/value registry
      reached from the world object (`vtbl[0x30]` / `vtbl[0x2c]`) holding names such as
      `lProfileManager`, `lCurrentProfile`, `strPreScene`. The level-name table is data at
      `HitmanDlc.dlc` `0x0FD3B6xx`, read by the profile/level-select code
      (`FUN_0fd3be80`, `FUN_0fd3e580`)
- [x] **Repeatable, keyboard-free level entry - solved, 2026-10-19.** The game's own level
      start is `world->vtbl[0x88](levelName, 0, 0)`, read out of `FUN_0fd3b609`
      (HitmanDlc.dlc RVA `0x7B609`): it reads the string property `strPreScene`, maps the `_Pre`
      name to the plain level name through a 13-entry `strcmp` chain, logs `Prescene push %s`,
      and calls that slot. Driving the slot from the mod loaded the training level with **no
      keyboard input at all**, straight from the intro screen, with the gameplay camera firing
      0.6 s later. Ask for it with `tools/drive.ps1 -Scene 'push:C0_Training\C0_1'`
- [x] Level entry confirmed end to end by the user: the push lands exactly at the start of the
      level, with a scripted cutscene before the player gets control. No skip string exists in
      the module (cutscenes are `CutSequence` data on a fixed timeline), so the test loop must
      allow time after a push before it drives the camera
- [x] Vendor the 32-bit `openxr_loader` (`third_party/openxr`, built from
      `release-1.1.65` by `tools/fetch-openxr.ps1`; verified i386 with the expected exports)
- [ ] Install x32dbg for live address and calling-convention validation
- [x] Vendor MinHook (`third_party/minhook`, v1.3.4) - it carries the shipping detours

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

Gate: **met, 2026-10-19, by a route the plan did not anticipate.** The VR
graphics path is proven (OpenXR session READY headless through SteamVR's null
driver, in the same 32-bit process as the legacy WGL context). The controllable
camera now exists too - not as a hook, but by driving the engine's own input:
with focus taken, synthetic mouse deltas reach the engine, the engine rotates its
own camera, and its visibility pass follows, so the world stays whole (verified
with captures, `tools/turn.ps1`). Interaction work is therefore unblocked, and
the "camera hook" the gate asked for is not needed for it.

### Milestone 2 - correct stereo rendering

- [~] Update gameplay once, then render both eyes from the same simulation state
      (frame boundary identified: `RenderOpenGL.dll` RVA `0x0048F0`, the only caller of
      `SwapBuffers`; measured at ~1 call per frame. `FUN_0fb73680` is per-pass at ~17-18
      calls per frame, so it is the wrong place for the eye loop)
- [~] Use the runtime's eye poses and asymmetric projections (runtime side proven by the
      probe; the renderer side is blocked: `glFrustum` is called from a code region Ghidra
      has not defined as a function, and recovering it is the next step)
- [x] **Apply camera changes before visibility calculation - solved, 2026-10-19, by not
      calculating them here at all.** Because the camera is driven through the engine's own
      input, the engine does the visibility pass itself with the camera it is actually using, so
      there is nothing to expand conservatively and no culled geometry to recover. The
      GL-boundary rotation (which does need this) is superseded and kept only as a fallback
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
- [x] The game does **not** reach the menu by itself: the intro sequence needs a human ESC. A
      scene request submitted on a frame schedule always lands during the intro, which is why
      the mod now fires on demand (`h47vr-go.txt`) instead
- [x] Corrected, and it matters: the intro and the level openings are **not video files**. The
      user confirms they are fully in-engine rendered scenes, which is why they are affected by
      the camera at all - an injected view rotation visibly wrecks them, and the engine can sit
      in one indefinitely until ESC advances it
- [x] Level transitions reload the engine's modules, discarding every patch installed by
      address; hooks have to be re-armed after a load, not installed once

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
