# Handoff

Resume work from here. This supersedes every earlier handoff.

## Mission

Native 32-bit x86 VR mod for the Steam release of *Hitman: Codename 47*
(appid 6900, buildid 251800). Plan of record is `PLAN.md`, with a living
progress checklist at the top of it - **update that checklist in the same commit
as the work**.

Repo: `C:\Users\titan\Desktop\Github_Projects\Mine\Htman-Codename-47-VR-Mod`
(remote `FractalEngineer/Htman-Codename-47-VR-Mod`, branch `main`).
Repo state: verify with `git status --short --branch`; this handoff predates its
own commit. At the start of this session `origin/main` matched local `main` at
`5baaa1d`, and this pass adds two local commits: the initial candidate fix and
the follow-up that records the negative isolated test. Do not assume the working
tree is still clean or that the new commits were pushed.
The game may or may not be running; it keeps rendering while the desktop is used,
so a session is free to launch it and walk away.

## Where things stand (2026-10-20)

**The long-standing blocker is gone.** The game can be driven into a level with
no keyboard input, and `_ZCAMERA_MouseConCam` has now been observed firing in a
loaded level - the Milestone 1 signal this project spent weeks chasing.

**The camera is now driven from inside the game, with no focus and no keyboard.**
`h47vr-look.txt` posts `WM_MOUSEMOVE` into the engine's own queue
(`src/core/src/look_driver.cpp`) - the same message a real mouse delivers, minus
the operating system's routing, which is what makes focus irrelevant. The engine
rotates its own camera and its visibility pass follows, so nothing is culled. A
`SetCursorPos` hook swallows the engine's cursor warp, so a turn no longer steals
the desktop pointer. A request is either relative pixels (`dx=`/`dy=`) or an
absolute aim (`yaw=`/`pitch=`). `src/core/src/camera_readback.cpp` now hooks
`MouseConCam`'s per-frame update and reads the live engine orientation from the
offset vector at `+0x2d`/`+0x31`/`+0x35`; `h47vr-camread.txt` carries the raw
pose and `h47vr-lookstate.txt` says whether the result came from `readback` or
the pre-MouseConCam pixel fallback. The measured scale is **0.1289156 deg/px**
(100 px = 12.8915 degrees, 450 px = 58.0120), and absolute recentre now returns
to the actual starting pose. This is the mechanism the headset will drive.

**Side-by-side stereo is working, 2026-10-19.** `RenderOpenGL.dll` RVA `0x3680`
is the face-list draw consumer, not the gameplay update. When a perspective
projection is current, `stereo_renderer.cpp` calls that consumer twice: once in
the left half-viewport with eye-space X shifted by `+IPD/2`, then in the right
half with `-IPD/2`. Projection X is scaled by 0.5 so vertical FOV and half-screen
aspect stay correct. The training capture shows clear parallax between the two
eyes, keeps the HUD and subtitles in each eye, and still reports 60 FPS. This is
a fixed-IPD monitor prototype using the user's 67 mm IPD at 100 game
units/metre; runtime eye poses and asymmetric frusta now feed this path and
await a live headset.

**The OpenXR pose backend is integrated behind a gate, 2026-10-19.** Creating
`h47vr-openxr.txt` makes the render thread initialize OpenXR on the game's
current WGL context, create a LOCAL reference space and locate predicted stereo
views. Runtime eye X positions replace the fixed IPD when valid. On this run,
Virtual Desktop initialised cleanly but `xrGetSystem` returned
`XR_ERROR_FORM_FACTOR_UNAVAILABLE` because no headset was streaming; the game
kept running on the 6.7-unit fixed fallback. If presentation setup is
unavailable, the backend still locates poses and submits no layer. The completed
presentation path is described below.

**Runtime FOV and head orientation are wired into the game, 2026-10-19.** The
stereo eye pass now receives each view's four FOV tangents. The proxy remembers
the engine's perspective near/far planes and replaces the desktop half-viewport
scale with a per-eye `glFrustum` for runtime views. Predicted head yaw drives
the engine's own mouse path through `look_driver.cpp`; pitch and roll are applied
directly to stereo eye geometry. The final physical tuning uses a 0.15-degree yaw
deadband and a 96-pixel per-frame cap. Pitch was deliberately removed from the
MouseConCam path because it moved the close follow-camera pivot vertically and
caused floor sinking. The no-HMD path still degrades cleanly and keeps fixed-IPD
stereo running.

**The gameplay camera can now sit in the head, 2026-10-19.** MouseConCam has two
fixed target follow distances in the supported build: 150 game units normally
and 100 while leaning. `first_person_camera.cpp` replaces both immediates while
`h47vr-firstperson.txt` exists, leaving the engine to perform its own transform,
collision and visibility work. The 3-unit default is the in-head test position.
Live verification moved the published offset from `150` to `3`, produced an
11.51 image difference from the normal follow view, held stereo at 60 FPS, and
returned to the original `150` offset and a 1.75 difference when disabled. Head
hiding, eye-height calibration and the final gameplay/head camera split remain.

**Theater/stereo selection is working on the desktop path, 2026-10-19.** A
MouseConCam update is treated as gameplay only for 250 ms. During the intro,
menus and scripted cameras the stereo renderer draws one full viewport pass;
when fresh MouseConCam updates resume it switches to two eye passes.
`h47vr-stereo-state.txt` reports `mode=off|theater|stereo`, gameplay freshness
and both pass counters. The live transition was verified: the intro reported
`mode=theater gameplay=0`, and the training level switched to
`mode=stereo gameplay=1` only after MouseConCam began updating, at 60 FPS. The
headset path submits that single image as a flat virtual-screen layer.

**Real OpenXR frame layers now submit, 2026-10-19.** Under SteamVR's null HMD,
the backend created two 1512x1680 OpenGL swapchains, attached every image to an
FBO, acquired/waited/released images in the required order, and submitted both
layer types. Theater mode uses one head-relative quad 1.8 m forward; gameplay
uses the runtime's two
poses and FOVs in a projection layer. The desktop SBS frame is blitted only when
native eye rendering is unavailable. The game remained at 60 FPS, published eye
X as approximately +/-0.0315 m, and toggled off cleanly.

**Gameplay now renders directly into the OpenXR eye images, 2026-10-20.**
`stereo_renderer.cpp` begins the XR frame before the first perspective draw.
`openxr_pose.cpp` locates the runtime views, acquires both swapchain images, and
builds depth-complete FBOs; `gl_probe.cpp` binds the matching target and
installs the per-eye asymmetric projection for each half of the existing
face-list replay. `SwapBuffers` submits the prepared projection layer directly,
without copying the desktop SBS frame. If native acquisition or binding is
unavailable, that frame falls back to the SBS blit. Under SteamVR's null HMD the
training level completed 79,588 stereo pairs at approximately 60 FPS; a 256x256
centre readback showed mean luminance 0.1713/0.1629 and a 0.0446 left/right
difference, proving non-black, non-identical eye images. A physical headset is
still needed for lens, convergence, comfort and final sharpness.

**Physical Quest pass, 2026-10-20.** VirtualDesktopXR reported 2412x2596
swapchains and approximately 51-degree inward / 37-degree outward per-eye FOV.
The live eye vectors initially exposed a coordinate bug: raw LOCAL-space X
changes as the head rotates, so it is not IPD. `openxr_pose.cpp` now subtracts
the eye midpoint and rotates the relative vector into head-local space. Across
a range of head yaw and pitch, the rendered offsets stayed at
`-0.0332 / +0.0332 m` on the user's 67 mm IPD.

The physical pass also produced these verified fixes:

- MouseConCam's four sway/bob oscillators are zeroed while first-person mode is
  active, removing most random camera shake.
- MouseConCam yaw remains the engine-facing turn path. The head yaw correction
  cap was raised from 18 to 96 pixels per frame and the deadband reduced to
  0.15 degrees; the user reported the fast-motion lag fixed.
- Headset pitch no longer goes through MouseConCam. Feeding pitch through mouse
  deltas moved the close follow-camera pivot downward and caused a slow sink
  through the floor. Pitch is now applied to the stereo vertices at the GL seam;
  the user confirmed the sink was resolved.
- Head roll is applied at the same vertex seam, around the eye forward axis. The
  first sign was reversed; it was flipped for the final build, but the user
  stopped headset testing before confirming that last change.
- Theater mode is now a head-relative quad 1.8 m forward at the current head
  position and orientation, instead of a hard-coded 1.5 m world height. The user
  confirmed the theater height and rendering fix.

**Open blocker: peripheral geometry is still culled, 2026-10-20.** The gray
region around the training corridor is culling, not empty level space. The user
confirmed this from the desktop view. An earlier attempt to blame the polygon
clipper at `HitmanDlc.dlc+0x93670` was disproved by an isolated A/B test and must
not be treated as a fix.

What was verified:

- Native training view reads 45 degrees horizontal in the renderer view object,
  producing a 34.52-degree vertical `glFrustum`.
- `tools/widefov.ps1` can replace the desktop SBS projection with 122 degrees
  horizontal / 107.07 degrees vertical, reproducing the wide headset view.
- With that **identical** wide projection, enabling the `0x93670` no-op path
  changed the frame by `0.00` and both captures had the same 192 sampled colours.
- Enabling the existing per-group `h47vr-clipall` byte under the same projection
  also produced `0.00`.
- Submitted vertices did not jump at the clip toggle: approximately 1,840 per
  frame before and 1,825 immediately after. The face list was already reduced.

Therefore `0x93670` is downstream of the visibility decision or only performs
geometric clipping on an already-culled group. The wide projection alone changes
the image, which caused the earlier false-positive comparison. The mod keeps the
clipper bypass as an explicit desktop diagnostic only; valid OpenXR poses no
longer enable it.

The renderer view object is still useful for reproducing the wide final
projection. `RenderOpenGL.dll` RVA `0x28A40` reads the horizontal FOV at
`+0x16E`; `tools/widefov.ps1` exposes that without a headset. The next target is
the face-list construction before `Clip`: `CamUpdate` calls the per-geometry
object's virtual `+0x398`, then tests whether the returned list is non-empty.
Tracing that path requires capturing the correct live object/vtable after a level
load. The stable `CamUpdate` stack argument is not itself a valid vtable object;
reading its first dword yields unwind/message helpers, so do not repeat that
dead-end assumption.

Experiments already tried and ruled out:

- Expanding the intercepted `glFrustum` by 4.25x did not affect the visible face
  list. It only changed the pre-eye projection and damaged opening/cutscene
  rendering, so the change was removed.
- Sending `CameraFOV` through the console dispatcher returned 0 and did not fix
  the view.
- A direct call through the world property registry with a string value crashed
  `EngineData.dll`; the probe was removed.
- The serialized `CameraFOV` double is 67.4 at `HitmanDlc.dlc+0x30054` in this
  build. A temporary live write to 110 did not remove the culling and left only
  floor and lower walls visible. The game was closed, restoring the value.
- `_ZLIST_CheckVisible` and `_ZGEOM_AdoptVisibility` are descriptor initializers,
  not boolean cull predicates. Their helper functions at `0xFE0DE60`,
  `0xFE19080`, `0xFE0E5D0`, and `0xFE0E1A0` are generic event/message handlers.
  The polygon clipper at `0x93670` was tested under an identical wide projection
  and did not recover geometry, so the real rejection still precedes it.

Do not repeat the projection overscan or property-registry calls. The useful
chain is now `FUN_0fd56120` (`CamUpdate`) -> `FUN_0fd53c50` (`Clip`) ->
`FUN_0fd53670` (polygon clipper). The old `FUN_0fe777f0` address was a no-op;
`FUN_0fe7b730` is a bounds helper, not the visibility decision.

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

**The camera is driven from the mod, and the user has seen it move.** The engine
transforms its own geometry, so the view cannot be moved through the GL matrix
pipeline; it is moved where the vertices are, at the proxy's
`glVertex3fv`/`glVertex3f` slot. `h47vr-cam.txt` carries `yaw=.. pitch=..`, about
2,900 eye-space vertices per frame are rotated by it, and the user confirmed the
view turns. Two rounds of work went into this: the first rotation was built in
GL's column-major order but applied row by row, which stays orthonormal and so
never looks broken, only subtly wrong - exactly what the user reported.

**What that rotation cannot do, and the answer to it: the engine culls before we
see anything.** It discards geometry outside the frustum of the camera *it*
believes it has, so a large injected yaw shows the scene with pieces missing -
the user's words were "we're just completely clipping". Rotating vertices cannot
bring back what was thrown away, and finding the transform to patch was the wrong
fix. The right one is to drive the input the engine already reads: with focus
taken, mouse deltas rotate the engine's own camera and its visibility pass
follows, so the world stays whole. That is verified and is now the primary route
(see the next section); the GL-boundary rotation is superseded, kept only as a
fallback for the sub-degree offsets a head pose actually needs.

## The tooling can now see and drive the game

Two capabilities landed on 2026-10-19 that change how everything above should be
verified, and both correct an old "dead end" note:

- **The picture is readable.** `tools/capture.ps1` grabs the game window with
  `PrintWindow`/`PW_RENDERFULLCONTENT` - a clean 800x600 frame, no focus stolen,
  works with the game behind other windows - and `-Compare` reports a mean
  absolute per-channel difference between two shots. The noise floor on a static
  scene is about 6 (camera sway), so anything clearly above that is a real
  change. Questions about what the player sees no longer need the player.
- **Focus can be taken.** A plain `SetForegroundWindow` is refused, which is why
  synthetic input never landed. `Focus-GameWindow` in `tools/Keys.ps1` now
  attaches to the current foreground thread and to the target's own thread first,
  which lifts the restriction: verified, the game reports as foreground.

With focus, input works - and that is the camera control this project has been
missing:

```powershell
./tools/turn.ps1 -Dx 220 -Verify     # turns the camera right, ~20 degrees
./tools/turn.ps1 -Dy -120            # looks up
```

**Driving the engine's own camera this way is the answer to the culling problem.**
The engine reads the mouse as deltas and rotates its own camera, so its
visibility pass and its geometry transform both follow and the world stays
whole - verified with captures: `turn.ps1` moved the view (mean difference
15.8-25.6 against a ~6 noise floor) and the frames show stairs, floor, walls and
the player model all present, with no missing geometry. Rotating the geometry at
the GL boundary instead leaves large empty regions, because the engine has
already discarded everything outside the frustum it believes in.

### The same turn, from inside the game and with no focus at all

`turn.ps1` needs the foreground, which is a bad fit for a headset and worse for a
test loop. It is no longer the primary route. The engine reads the mouse **only
from window messages** - there is no `GetCursorPos` and no DirectInput import
anywhere in the install, which is worth knowing before hunting for one:

```powershell
./tools/look.ps1 -Dx 220 -Verify     # same turn, no focus, no synthetic input
```

`RenderOpenGL.dll`'s window procedure (RVA `0x2A050`) routes `WM_MOUSEMOVE` and
`WM_NCMOUSEMOVE` to the handler at RVA `0x29AC0`, which:

- subtracts the client-area centre from the cursor position, so the message's
  offset from the centre *is* the delta;
- warps the cursor back to that centre with `SetCursorPos`;
- accumulates the delta into the input interface, `g_pSysInterface + 0xa95`
  (x) and `+0xa99` (y), as floats;
- publishes the crosshair position at renderer object `+0x3e` (x) and `+0x42`
  (y), normalised to the window and clamped to +/-0.5.

`src/core/src/look_driver.cpp` therefore posts `WM_MOUSEMOVE` into the engine's
own queue - the same message a real mouse delivers, minus the operating system's
routing, which is what makes focus irrelevant. Verified with the game sitting in
the background: 15 postings, 0 failures, capture difference 20.2-20.5 against a
1.8 static control, and an exactly reversed turn returning the frame to 2.4.

The handler also re-centres the cursor with `SetCursorPos` after every move,
which dragged the real pointer into the game window. The mod hooks
`user32!SetCursorPos` and swallows only the call whose return address is inside
that handler (`0x29AC0`..`0x29E50`), so the engine still stores its new position
and deltas and the desktop pointer is left alone. The swallow counter rises by
exactly one per posted message; when reading captures, a stray pointer move
during a test is therefore the user, not the game.

A turn can also be an absolute aim rather than a pixel count:

```powershell
./tools/look.ps1 -Yaw 90         # 90 degrees from where the driver started
./tools/look.ps1 -Pitch -10      # ten degrees up
```

The engine has no look-at entry point, so absolute requests are still applied as
pixel deltas. They are now **closed against the engine's pose**, not dead
reckoning: the MouseConCam update runs first, `camera_readback.cpp` samples the
offset vector it just wrote, and the driver posts only the difference to the
requested yaw/pitch. `h47vr-lookstate.txt` reports `source=readback` with
`rawyaw`, `rawelevation`, the camera object and its epoch. `-DegPerPixel` remains
available for the fallback path before MouseConCam becomes active; it is not
needed during gameplay.

The readback vector is the engine's camera offset behind the player. The values
are read after the update at `+0x2d`/`+0x31`/`+0x35`; yaw is
`atan2(-x, -z)` in degrees and engine elevation is `atan2(y, horizontal)`.
`tools/look.ps1` exposes pitch positive down, so the state file negates engine
elevation. A controlled 100 px turn produced 12.8915 degrees, 450 px produced
58.0120 degrees, and a -100 px turn returned exactly to -90.000 yaw / 0.000
elevation. A live target of yaw 10 / pitch -5 settled at 10.0554 / -5.02771,
within one pixel of rounding, and recentering to 0 / 0 returned to 0 / 0.

The image- and memory-based calibration attempts remain useful history, but do
not rebuild them now that the engine pose is readable:

- **Floor-plane homography.** `tools/calibrate-look.py` computes the rotation
  between two captures from the floor, and it is correct on a synthetic 9 degree
  warp (9.013). On real frames it returns ~1 degree per 100 px with a large
  off-axis residual - the fit is not the floor. The camera sways on four sine
  oscillators and orbits the player, so consecutive frames are not a clean plane
  homography, and only ~35 of ~50 features survive RANSAC.
- **Tile repeat.** The training cell's floor is a square grid, so a 90 degree
  turn should reproduce the pattern (rotated). A sweep of 100..1400 px in 100 px
  steps shows no dip at any point: the cell is too self-similar and the camera
  orbit changes the framing as much as the rotation.
- **Process-memory float scan.** `tools/scan-yaw.py` snapshots writable memory,
  injects the same turn twice, and keeps floats whose delta is identical - then
  repeats with a doubled turn to catch scaling. No address survives: the
  candidate sets do not even overlap between runs. The yaw is therefore not a
  scalar angle in a stable field; expect a basis vector or a quaternion, which
  is consistent with the earlier failure to drive the `CamUpdate` basis.

An absolute request in a newly loaded level can arrive before MouseConCam is
active. It is queued rather than posting deltas the engine may ignore; once
`h47vr-camread.txt` starts updating, the driver takes that first real pose as
the recenter reference and applies the target from there. Relative `dx`/`dy`
requests still act immediately and use the pixel integral as their fallback.

The windowed path only acts while the renderer holds the cursor (`renderer+299`,
set by the `ClipCursor` wrapper at RVA `0x2B640`), which happens in gameplay.
The fullscreen path at the same handler reads its own remembered cursor position
(`+0x46`/`+0x4a`) instead and edge-wraps, so posted messages drive that too.

## Working commands

```powershell
# deploy the current build + launch via Steam (the deploy step matters: Steam
# runs whatever is in the game directory, so a bare launch runs the OLD build)
./tools/enter-level.ps1 -BurstSeconds 0 -KeepOpen

# into a level, any time after that, no relaunch and no keyboard
./tools/drive.ps1 -Scene 'push:C0_Training\C0_1'

# submit an arbitrary engine command after the level is running
./tools/drive.ps1 -Scene 'cmd:<name>=<arg>'

# full loop: build, install, launch, watch the log, close
./tools/iterate.ps1 -Scene C0_1 -CloseGame

# turn the engine's own camera - no focus, no synthetic input, no cursor theft
./tools/look.ps1 -Dx 220 -Verify        # relative, in pixels
./tools/look.ps1 -Yaw 90 -Verify        # absolute, in degrees from the start
./tools/look.ps1 -Yaw 0                 # recentre exactly
./tools/firstperson.ps1 -Distance 3     # move MouseConCam into the head
./tools/firstperson.ps1 -Off            # restore the follow camera
./tools/stereo.ps1 -IpdMillimeters 67   # side-by-side stereo prototype
./tools/stereo.ps1 -Off                 # back to the normal view
./tools/openxr.ps1 -Enable              # initialize OpenXR pose + presentation
./tools/openxr.ps1 -Enable -RuntimeJson 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win32.json'
./tools/openxr.ps1 -Off                 # leave headset initialization off

# calibration experiments (see Dead ends before using them in anger)
python tools/calibrate-look.py before.png after.png --px 100 --region 260,150,640,330
python tools/scan-yaw.py --pixels 100 --save tools/_shots/scan100.txt
```

Every level is reachable: `C0_Training\C0_1`, `C1_HongKong\C1_1..C1_4`,
`C2_RotterdamHarbor\C2_1/C2_2`, `C3_BudapestHotel\C3_1`,
`C4_ColombianRainforest\C4_1..C4_3`, `C5_Sanitarium\C5_1/C5_2`.

The request file `<game>\h47vr-go.txt` is polled at frame boundaries and
consumed on sight. Its payload decides what happens:

| Payload | Meaning |
| --- | --- |
| `push:<level>` | **the working level load** (game's own slot) |
| `cmd:<name>=<arg>` | submit an arbitrary engine console command; usable after a level loads |
| `<mode>:<name>` | call the EngineData command entry directly with that mode |
| anything else | console `startscene <line>` (dead end, kept for reference) |

Control files next to the game (all optional, delete to disable):

| File | Effect |
| --- | --- |
| `h47vr-look.txt` | `dx=<px> dy=<px> [steps=<n>] [rate=<frames>]` - turn the engine's **own** camera by posting `WM_MOUSEMOVE`; no focus needed. Consumed on sight, so one write is one turn (`tools/look.ps1`) |
| `h47vr-camread.txt` | live MouseConCam pose: `yaw`, engine `pitch` (elevation, positive up), the player-relative offset, camera pointer, epoch and update count |
| `h47vr-stereo.txt` | while present, render perspective face-list passes side by side; `ipd_mm=<millimetres> units_per_m=<scale>` sets physical eye separation (`tools/stereo.ps1`) |
| `h47vr-openxr.txt` | while present, initialize OpenXR on the game's WGL context and use predicted eye X positions when available (`tools/openxr.ps1`) |
| `h47vr-openxr-runtime.txt` | optional process-local `XR_RUNTIME_JSON` path for testing another runtime without changing the machine registry |
| `h47vr-widefov.txt` | `fov_deg=<degrees> [clip=0]` - diagnostic wide desktop projection; `clip=0` isolates the projection from the unproven polygon-clipper experiment (`tools/widefov.ps1 -NoClipBypass`) |
| `h47vr-firstperson.txt` | `distance=<game units>` - replace MouseConCam's 150/100-unit follow targets so the engine places the gameplay camera in the head (`tools/firstperson.ps1`) |
| `h47vr-stereo-state.txt` | published state: `mode=off\|theater\|stereo`, `gameplay=0\|1`, eye separation and both pass counters |
| `h47vr-cam.txt` | `yaw=<degrees> pitch=<degrees>` - the injected camera rotation |
| `h47vr-glprobe.txt` | comma list of GL exports to intercept, or `all` / `none` |
| `h47vr-no-glprobe.txt` | shorthand for `none` |
| `h47vr-glmatrix.txt` | while the file exists, log every changed `glLoadMatrixf` matrix |
| `h47vr-stack.txt` | dump a few stacks at `glVertex3fv`, to re-find the engine's transform |
| `h47vr-camobj.txt` | world probe: reports every camera candidate's `+0x106` triple, its `+0x0DA` basis, and the offsets the engine itself rewrites |
| `h47vr-camwrite.txt` | `yaw=<deg> pitch=<deg> [at=frame\|frustum] [index=<n>] [dry=1]` - rotates the identified camera object's basis in memory |

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
- **In-process camera turn, no focus.** `WM_MOUSEMOVE` posted into the engine's
  own queue drives the same handler a real mouse does. Verified with the game in
  the background: 15/15 messages posted, frame difference 20.2-20.5 against a
  1.8 static control, exact reversal back to 2.4, and the log records the
  foreground window so the claim is checkable on every run.
- **Cursor guard.** The engine's `SetCursorPos` re-centre is swallowed when the
  call comes from its own mouse handler (`RenderOpenGL` `0x29AC0`..`0x29E50`).
  The swallow counter rises by exactly one per posted message; turns no longer
  move the desktop pointer.
- **Live camera readback and absolute aim.** `camera_readback.cpp` hooks the
  MouseConCam per-frame update and reads the offset vector it produced. The
  measured scale is 0.1289156 deg/px. Absolute requests are closed against this
  live pose; `h47vr-lookstate.txt` reports `source=readback`, and `yaw=0
  pitch=0` returns to the engine's actual starting pose. Before MouseConCam
  becomes active, absolute requests wait for readback; relative turns use the
  pixel integral.
- **Side-by-side stereo loop.** The renderer's face-list consumer (`0x3680`) is
  called twice only while the perspective projection is current. Each eye gets
  its own half-viewport, a 0.5 X projection scale and an eye-space X offset from
  `h47vr-stereo.txt`; the current profile uses 67 mm and 100 game units/metre
  (6.7 units). The original viewport and matrix mode are restored after each
  pass. The first capture showed 21.46 mean channel difference from the normal
  view and 7.41 between left and right at the initial 10-unit test separation,
  with 60 FPS retained. Simulation is not duplicated because this function
  consumes already-built draw lists.
- **Runtime eye projection and head feed.** When OpenXR views are valid, eye X
  positions replace the fixed separation, each view's four FOV tangents rebuild
  the projection through `glFrustum`, and headset yaw drives MouseConCam through
  the no-focus input path. Physical Quest testing validated the 67 mm head-local
  eye separation and asymmetric FOV. Headset pitch and roll are applied at the GL
  vertex seam to keep the first-person pivot stable.
- **First-person MouseConCam position.** The supported build's follow-distance
  immediates are patched only while `h47vr-firstperson.txt` exists. A 3-unit
  target moved the live offset from 150 to 3 with the engine still performing
  collision and visibility; disabling it restored 150 and the normal view.
- **Theater/stereo auto-selection.** MouseConCam freshness gates immersive
  duplication. The intro stayed `theater`; gameplay changed to `stereo` after
  the readback became live, and the frame cadence remained 60 FPS.
- **OpenXR pose initialization from the game.** The gated backend initializes
  the 32-bit Virtual Desktop runtime inside the game process, creates an
  instance with `XR_KHR_opengl_enable`, reaches the runtime system query and
  degrades cleanly when no HMD is attached. This proves the in-game client side
  of the bridge; presentation was subsequently proven under SteamVR's null HMD.
- **OpenXR presentation and native eye rendering under the null HMD.** Two
  accepted 1512x1680 GL swapchains, depth-complete per-image FBOs, theater quad
  and gameplay projection layer submit with correct acquire/wait/release
  ordering and no runtime errors at 60 FPS. Gameplay face-list draws render
  directly into each acquired image. Pixel readback measured 0.1713/0.1629 mean
  luminance and a 0.0446 left/right difference. The desktop SBS blit remains the
  fallback when native XR targets are unavailable.

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
| MouseConCam follow-distance immediates (150 / 100) | `0x2714F` / `0x27158` |
| `_ZCAMERA_BaseCamera` / `_MainCameraControl` / `_NewFollowCam2` / `_ScopeCamera` | `0x020E80` / `0x023940` / `0x029700` / `0x02D060` |
| `_ZLIST_SortCameraControls` | `0x023430` |
| World `CamUpdate` / `Clip` / polygon clipper | `0x96120` / `0x93C50` / `0x93670` |
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

`RenderOpenGL.dll` (loads at `0x0FB70000`): renderer view setup `0x028A40`
(reads the view object's horizontal FOV at `+0x16E`), presentation `0x0048F0` (~1/frame,
calls `glFinish` and `SwapBuffers`), face-list draw consumer `0x003680`
(`__thiscall(self, list)`, ~21 calls/frame), `glDrawElements` candidates
`0x006590` and `0x006270`, 2D UI passes `0x004930`, `0x005300`, `0x005A10`,
`0x005F20`, `0x002EC0`. The renderer vtable used by the present function starts
at `0xFB9F300`; its `+0x68` slot is the present/sync path and `+0x6C` is
`0x3680`. The stereo path hooks `0x3680` and replays perspective calls there.

Window and input, same module (found 2026-10-19 with the new
`tools/ghidra/DumpImportCalls.java`, which dumps call sites for any import by
name rather than only the GL ones):

| Item | RVA |
| --- | --- |
| Window procedure (routes messages, default cases call `DefWindowProcA`) | `0x2A050` |
| `WM_MOUSEMOVE`/`WM_NCMOUSEMOVE` handler: delta, cursor warp, accumulators | `0x29AC0` |
| Window create/restyle and first `SetCursorPos` | `0x2B8D0` |
| `ClipCursor` wrapper - sets `renderer+299`, gates the windowed mouse path | `0x2B640` |
| `SetCapture` + keyboard polling (`GetAsyncKeyState` into `interface+0xab6`/`+0xab7`) | `0x2B4F0` |
| Renderer window-manager object vtable | `0x30470` (`+0x194` is the mouse handler) |

Input state the engine publishes, none of it needing the mouse itself:
`g_pSysInterface + 0xa95`/`+0xa99` are the accumulated x/y pixel deltas (float),
`+0x19`/`+0x1d` the render size, `+0x38f1` the fullscreen flag; the renderer
object carries the crosshair at `+0x3e`/`+0x42` (clamped +/-0.5), the last
cursor position at `+0x46`/`+0x4a` with its previous value at `+0x4e`/`+0x52`,
`+0x5b` captured, and `+299` clip-enabled.

What reads those accumulators, found with `tools/ghidra/FindScalarRefs.java`
(scans instructions for a displacement; `DumpRefsTo` only works on addresses):

- `FUN_0fce6f20` (RVA `0x26F20`) is the **MouseConCam per-frame update**. It
  reads the crosshair through the handler id at `+0x81`, stores it at
  `+0x85`/`+0x89`, turns it into an aim offset with a `tan` term, and copies the
  input accumulators into its own saved copies at `+0x45`/`+0x49`/`+0x4d`.
  `+0x65`..`+0x71` are four sine sway/bob oscillators.
- The live camera offset is the float triple at `+0x2d`/`+0x31`/`+0x35`,
  normalized to the camera distance at `+0x51` and rotated from the input at
  the start of the same update. The readback hook samples it after
  `FUN_0fce6f20` returns. Yaw is `atan2(-x, -z)`; elevation is
  `atan2(y, horizontal)`. The engine's world yaw starts at an arbitrary value
  (the training level used -90 degrees), so the look driver records the first
  pose per camera object as its recenter reference.
- `FUN_0fce6bc0` (RVA `0x26BC0`) is the activation/mode handler beside it; it
  saves the same three accumulators on activation.
- Other readers of `+0xa95`/`+0xa99`: `FUN_0fce4bb0`, `FUN_0fcee9f0`,
  `FUN_0fceed30`, `FUN_0fd06840` (save/restore across a scene change),
  `FUN_0fd38190`, `FUN_0fdf0c70` (player action state), `FUN_0fe26d30`, plus two
  regions Ghidra has not defined as functions at `0x0fd45501` and `0x0fd459ad`.

The crosshair is therefore the *aim*: the camera's look offset comes from its
clamped screen position, while the player's facing comes from the integrated
mouse delta. The earlier dead reckoning matched that integral; the current
readback samples the rotated offset vector after each MouseConCam update, so
scripted turns and other camera movement no longer desync absolute aim.

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

**Not located, and no longer on the critical path: the engine's world-to-eye
transform.** Nothing is blocked on it any more - free look runs through the
engine's own input - so what is left here matters only for the eye loop, where a
per-eye offset has to be injected somewhere the engine's visibility pass cannot
cut away. Rotating a gameplay-side camera object's basis does nothing: the whole
set of `CamUpdate` objects was swept, one at a time, and no candidate's `+0x0DA`
changed the picture at all (differences 1.1-6.4 against a ~6.0 noise floor). The
vertex stack says why - `Clip` reaches `glVertex3fv` through `RenderOpenGL.dll`
(`+0x0292FA` -> `+0x004483` -> `+0x008A78`, the direct caller), so the transform is
applied inside the engine's OpenGL module, not the gameplay module. **Search
`RenderOpenGL.dll` if a per-eye offset turns out to need it.**

Established: the
modelview is always identity, the projection is a plain perspective with no
rotation or translation (checked in a loaded level), and `glMultMatrixf`,
`glPushMatrix`, `glPopMatrix`, `glRotatef` and `glScalef` are never called at
all. So the view is applied on the CPU before submission, and the engine culls
first. Without that transform, a rotation *we* apply cannot avoid the culling
cutting the scene away - which is exactly why free look drives the engine's input
instead, and why a per-eye offset is the only thing that still wants it.

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

- **Exact look calibration from images or from memory.** Three methods tried on
  2026-10-19, all failed: floor-plane homography (`tools/calibrate-look.py` is
  correct on a synthetic warp but real frames are not a plane homography - the
  camera sways on four sine oscillators and orbits the player); tile-repeat (the
  cell is too self-similar, no dip anywhere from 100 to 1400 px); and a float
  scan of process memory (`tools/scan-yaw.py` - no stable field, so the yaw is a
  basis or quaternion rather than a scalar). The exact value was obtained from
  the live MouseConCam offset instead; do not rebuild these methods.
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
- **Synthesised keyboard input - reopened, and now viable.** This was written off
  because the keys never landed, but the real cause was focus: `SetForegroundWindow`
  was being refused, so the events went to whatever window did have focus. With
  `Focus-GameWindow`'s AttachThreadInput route the game genuinely takes the
  foreground, and **mouse deltas now demonstrably drive the engine's camera**
  (`tools/turn.ps1`, verified by capture). Keyboard through the same path has not
  been re-tested yet, so do not assume ESC now works - but "input injection does
  not work here" is no longer true, and the mouse half is proven.
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
- **Screen capture is NOT impossible - the old note here was wrong.** `PrintWindow`
  with `PW_RENDERFULLCONTENT` (flag 2) returns the window's own contents: a clean
  800x600 frame, with the game behind other applications and without stealing
  focus. A plain `CopyFromScreen` of the desktop works too. Use
  `tools/capture.ps1`, which does both and has a `-Compare` mode that reports a
  mean absolute per-channel difference between two shots, so "did that change the
  picture" is answerable from here. What genuinely does not work is *input*:
  synthetic keys need the window to be foreground and `SetForegroundWindow` is
  refused, so the user still presses ESC for the intro.
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
./tools/widefov.ps1              # desktop A/B for the headset culling path
./tools/steamvr-headless.ps1     # -Action Enable|Disable|Status (null HMD)
./tools/ghidra-analyze.ps1       # headless analysis + anchor extraction
python tools/peutils.py <dll>    # exports with RVAs
```

Ghidra scripts in `tools/ghidra/`: `ExtractAnchors.java`, `DumpReferences.java`
(GL import call sites), `DecompileXrefs.java` (find + decompile whatever
references a string), `DecompileCallers.java`, `DumpRefsTo.java` (refs to an
address), `DecompileAt.java` (materialises a function Ghidra never defined, then
decompiles it - this is what cracked the level start), `DumpImportCalls.java`
(call sites of any import by name, which is how the mouse path above was found),
`FindScalarRefs.java` (every instruction mentioning a displacement value, which
is how the readers of `interface+0xa95` were found).
Run:

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
- Pictures work and are cheap: `tools/capture.ps1` (`-Compare` scores a change
  against the ~6 noise floor). The old "`PrintWindow` returns black" note was
  wrong - see the dead ends. Still prefer a numeric check when one exists.
- The proxy's matrix log is gated on `<game>\h47vr-glmatrix.txt`: the front end
  rewrites its projection and texture matrices every frame and would spend the
  whole budget before a level loads.
- The GL probe can be bisected without a rebuild: put the export names in
  `h47vr-glprobe.txt`, or `none` to turn the whole probe off.

## Immediate next steps

Milestone 2 continues here. Native OpenXR eye rendering, physical 67 mm IPD,
head tracking and theater placement are working. Peripheral culling remains the
primary blocker.

1. **Find the pre-Clip face-list culling.** `tools/widefov.ps1 -Degrees 122
   -NoClipBypass` reproduces the wide desktop view. The clipper at `0x93670` and
   the `h47vr-clipall` byte have both been ruled out under that identical
   projection. Trace the virtual `+0x398` call that `CamUpdate` makes before
   testing whether the returned list is non-empty. Capture the correct live
   geometry object and vtable after a level load; do not use the stable stack
   argument as an object vtable. The expected outcome is that the gray
   peripheral region fills with real corridor/room geometry.
2. **Confirm head roll direction.** The final build flips the roll sign so the
   scene rotates opposite the headset roll. This has not been tested since the
   last sign change. Confirm left and right tilt, then adjust only the sign in
   `stereo_renderer.cpp` if needed.
3. **Finish the physical visual pass.** Confirm lens-correct convergence,
   comfort, sharpness and HUD/UI coverage. The native path currently captures
   perspective face lists; orthographic gameplay UI may still need an explicit
   per-eye overlay.
4. **Finish first person.** Hide the player's head while keeping the body for
   shadows/reflections, calibrate eye height and near clipping, and validate that
   the direct pitch/roll path stays stable through crouching, leaning and scripted
   camera changes.
5. **Calibrate game units.** The working scale is 100 units/metre with the user's
   67 mm IPD. Validate it against physical level landmarks before tuning movement
   or eye height.
6. **Keep theater mode covered.** The head-relative quad is correct on the
   physical headset; verify text readability and transitions through menus,
   intro cinematics and in-game cutscenes after the culling work.
7. **Keep the renderer map honest.** `objectDrawA` (`0x6590`) remains inert, so
   the world path is the immediate-mode `0x3680` face-list consumer. Confirm its
   state restoration and transparent-pass ordering under heavier scenes, then
   install x32dbg if a calling convention needs direct confirmation.
