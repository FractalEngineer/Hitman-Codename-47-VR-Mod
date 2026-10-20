# Graphics Bridge

The plan's first technical gate: can a 32-bit process hold a legacy Windows
OpenGL context and an OpenXR session at the same time?

**Status: PASSED, headless.** The session gate passed on 2026-10-18. The
presentation follow-up passed under SteamVR's null HMD on 2026-10-19: two
1512x1680 OpenGL swapchains, a theater quad and a two-view gameplay projection
layer all submitted successfully.

**In-game follow-up, 2026-10-19:** the mod now contains a gated OpenXR pose
backend (`src/core/src/openxr_pose.cpp`). It initializes Virtual Desktop inside
the running game on the game's current WGL context. With no headset streaming,
`xrGetSystem` returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE` and the game continues
on its fixed 67 mm fallback. When views are valid, runtime eye X, asymmetric FOV
tangents and head orientation feed the stereo renderer and the engine's own
look-input path. The backend now creates GL swapchains and submits real frame
layers. The 2026-10-20 follow-up renders gameplay directly into those eye
images; a physical-headset validation pass remains.

## Gate result

`src/probe/main.cpp` builds to `h47vr-openxr-probe.exe`, and it walks the bridge
step by step, continuing after a failure so one run shows how far it gets.

Run headless against SteamVR (`XR_RUNTIME_JSON` pointing at
`steamxr_win32.json`, null driver enabled):

| Step | Result |
| --- | --- |
| Instance extensions | 41, from `SteamVR/OpenXR` |
| `XR_KHR_opengl_enable` advertised | yes |
| `XR_KHR_D3D11_enable` advertised | yes (fallback route remains available) |
| `xrCreateInstance` | ok, runtime `SteamVR/OpenXR 2.17.10` |
| `xrGetSystem` | ok, system `SteamVR/OpenXR : null` |
| View configuration | 1 config, 2 eyes at 1512x1680 recommended |
| Legacy WGL context | ok, `GL 4.6.0 NVIDIA 616.92` on an RTX 4070 Ti |
| `xrGetOpenGLGraphicsRequirementsKHR` | ok, min GL 4.3, max GL 4.6 |
| `xrCreateSession` (OpenGL Win32 binding) | **ok** |
| Session state | `XR_SESSION_STATE_READY` |
| `xrBeginSession` | **ok** |

SteamVR's own log confirms it treated the probe as a real OpenXR application:

```
Processing message VRMsg_ChangeAppType from 16196: h47vr-openxr-probe
AppInfoManager.ProcessDisconnected: Clearing application
  system.generated.openxr.h47vr probe.h47vr-openxr-probe.exe
```

So the architecture the plan depends on is sound: a 32-bit process can hold both
the game's legacy WGL context and a live OpenXR session. The D3D11 sharing
fallback is not needed.

Virtual Desktop was re-tested with the same probe and also creates instances
cleanly (`VirtualDesktopXR 1.0.10`). It only fails `xrGetSystem`
(`XR_ERROR_FORM_FACTOR_UNAVAILABLE`) while no headset is streaming, which is
expected and not a defect.

## The bug that made this look impossible

The first runs failed `xrCreateInstance` with `XR_ERROR_INITIALIZATION_FAILED`
against every runtime, in both architectures, with zero extensions enabled. It
was reported here as an environmental block. **That was wrong.**

```
// what the probe did
XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
createInfo.applicationInfo.applicationName = "h47vr probe";
// applicationInfo.apiVersion left at zero

// what it must do
createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
```

The loader accepts an unset `apiVersion`; the runtime then rejects instance
creation. The symptom is indistinguishable from a broken graphics bridge, and it
produced a false conclusion that the hardware was at fault.

Lesson recorded: when two independent runtimes fail identically at the *same*
step, suspect the caller before the environment.

## Runtime facts

| Item | Finding |
| --- | --- |
| Active 32-bit runtime | Virtual Desktop (`virtualdesktop-openxr-32.json`), runtime 1.0.10 |
| SteamVR 32-bit runtime | `SteamVR\bin\vrclient.dll`, genuinely i386, exports `xrNegotiateLoaderRuntimeInterface` |
| SteamVR 64-bit runtime | `SteamVR\bin\vrclient_x64.dll` |
| SteamVR null driver | `drivers\null\bin\win32\driver_null.dll`, enabled by `driver_null.enable` in `steamvr.vrsettings` |
| Loader | Vendored: `third_party/openxr/lib/x86/openxr_loader.dll`, built from `release-1.1.65` |
| Oculus / Meta runtimes | Registered but **stale**: the manifest files do not exist |
| Headset | Meta Quest 3, per `steamvr.vrsettings` (`oculus_virtualdesktop` driver) |
| GPU | NVIDIA RTX 4070 Ti, driver 32.0.16.1692 |

No `openxr_loader.dll` existed anywhere on the machine, in either architecture,
so `tools/fetch-openxr.ps1` clones the SDK at a pinned tag and builds the x86
loader with the Visual Studio toolchain, installing it under `third_party/openxr`
with the generated public headers and import library.

### Correction to the plan

The plan recorded "SteamVR ships a 64-bit loader only". That conflated the
OpenXR *loader* with the OpenXR *runtime*. SteamVR ships a real 32-bit OpenXR
runtime, which is what makes the headless gate possible.

## Headless operation

`tools/steamvr-headless.ps1` enables SteamVR's null HMD driver, starts SteamVR
and backs up `steamvr.vrsettings` first. `-Action Disable` stops SteamVR and
restores the original config. The null driver's own default `loadPriority` is
-999, so a real headset always wins when one is connected.

```powershell
./tools/steamvr-headless.ps1 -Action Enable
$env:XR_RUNTIME_JSON = 'C:\Program Files (x86)\Steam\steamapps\common\SteamVR\steamxr_win32.json'
./build/bin/probe/h47vr-openxr-probe.exe
./tools/steamvr-headless.ps1 -Action Disable
```

The probe builds into `build/bin/probe`, deliberately not `build/bin`: the
app-local `opengl32.dll` proxy lives there, and Windows would load it into the
probe, silently layering the mod underneath it.

### Side finding: the fingerprint gate proved itself on the wrong host

Before the probe was isolated, it loaded our proxy and the mod started inside a
program that is not the game. The gate refused:

```
[error] unsupported build (appid 6900 buildid 251800): 11 missing, 0 mismatched.
        No patches will be applied.
```

That is the "unknown builds receive a clear compatibility message" acceptance
test demonstrating itself on an unintended host, which is a stronger proof than
a deliberate test.

## What is not proven yet

- The physical headset still needs a final visual pass for lens correction,
  convergence, comfort and subjective sharpness. The runtime-side eye positions,
  asymmetric tangents and head tracking have already been exercised.
- Physical Quest validation confirmed the 67 mm head-local IPD, head tracking
  and theater placement. Peripheral culling is now addressed at the engine
  camera, not the downstream polygon clipper: `HitmanDlc.dlc` RVA `0x92040`
  rebuilds the camera's visibility state from its first FOV argument. The
  renderer publishes the symmetric horizontal FOV covering both OpenXR eye
  frusta, including per-eye translation at the near plane. The submitted
  per-eye projection remains the runtime FOV.
- A 6-degree-per-side visibility-margin experiment widened the engine culling
  camera further but produced no visible improvement in the physical headset.
  The margin path has been removed, restoring the exact raw runtime eye-union
  calculation from `b4c792c`. The remaining edge band needs a different cause
  than a symmetric FOV shortfall.
- The XR mirror reproduces the new culling clue: straight ahead is coherent, but
  pitching the engine camera up 45 degrees loses the lower half of the submitted
  image. The remaining defect is orientation-dependent visibility. The engine
  visibility state and the final headset projection are not yet coincident under
  the GL-seam pitch path. The native pitch feed produced coherent ceiling and
  floor views without half-frame loss and was confirmed fixed by the user in the
  live Quest session. It is now enabled for the active test profile.
- The legacy renderer emits display-encoded color. XR presentation therefore
  prefers `GL_SRGB8_ALPHA8`; `GL_RGBA8` is a fallback for runtimes that do not
  offer it. Under VirtualDesktopXR the sRGB swapchain was accepted at
  `0x8C43`, avoiding the brightness lift produced when display-encoded values
  were interpreted as linear.
- The null-HMD run proves native per-eye rendering and layer submission. The
  acquired 1512x1680 images have 24-bit depth attachments and receive the
  face-list replay directly. A centre readback measured mean luminance
  0.1713/0.1629 and a 0.0446 left/right difference. The desktop side-by-side
  blit remains as the fallback path.
- A physical Virtual Desktop/Quest session exercised the headset path: runtime
  eye positions, asymmetric FOV, tracking and theater placement were observed.
The wide-visibility path has been verified on the desktop and under SteamVR's
null HMD. A physical Quest pass found the corrected path practically usable but
with a residual edge band. Extra symmetric FOV did not change that band. sRGB
brightness parity has been confirmed. A faithful left/right/side-by-side mirror
of the submitted XR eye image now exists and has been used to reproduce
pitch-dependent culling. It blits the completed OpenXR swapchain image rather
than re-rendering the world, so projection, visibility and color remain the same
as headset submission.

## Presentation path

`SwapBuffers` comes from `gdi32.dll`, not from `opengl32.dll`. Interception
therefore belongs on `gdi32`, either by import table patching in
`RenderOpenGL.dll` or by a hook. The proxy remains the load point for the mod; it
is not the presentation hook.

The intended presentation split is theater mode for menus, the intro and
in-game cinematics, with immersive stereo only while MouseConCam confirms
gameplay. Theater mode should submit one flat screen layer rather than
duplicating those perspective frames per eye.

The desktop renderer now implements that selection: MouseConCam updates remain
gameplay-fresh for 250 ms, stale/menu/scripted-camera states draw one theater
pass, and `h47vr-stereo-state.txt` publishes the current mode. The headset layer
now consumes this state: theater submits a quad, gameplay submits a projection
layer. Gameplay begins the XR frame before the first perspective draw, acquires
both eye images, and renders the existing face-list replay into their FBOs. If
native acquisition or binding is unavailable, the renderer reverts to copying
the desktop side-by-side halves for that frame.

`h47vr-mirror.txt` adds a debug presentation path after the layer has been
completed. Left and right layouts fit one swapchain image into the 800x600
window; `sbs` places both eye images side by side. The mirror preserves the
OpenXR sRGB swapchain format and can dump the resulting framebuffer through
`tools/mirror.ps1 -Capture`.

Head yaw no longer uses synthesized mouse messages. `openxr_pose.cpp` publishes
the predicted head pose, and `look_driver.cpp` adds the closed-loop yaw delta to
`g_pSysInterface + 0xA95` immediately before MouseConCam's native update.
Controller smooth/snap turning uses the same accumulator. Controller movement
uses the engine's per-key state; ESC is posted as a held window key pair because
a zero-duration pair is ignored. Menus additionally use the right stick as an
absolute pointer. The mod captures RenderOpenGL's window-input object and calls
its native move/down/up slots directly, because posted mouse messages are gated
off while MouseConCam owns input.

The live controller profile now uses corrected movement axes, 180-degree/second
smooth turn, directional stick-click snap, menu-button ESC, trigger fire,
A/B/X/Y actions, grip actions and left-stick-click run. The current world scale
is a provisional 110 units/metre. Eye alignment now uses the engine's own follow
target as the pivot, with `camera_lower=20` and `camera_forward=2.2`; neutral and
plus/minus 30-degree headless readback keep the camera position fixed. Gameplay
stereo applies a configurable near-plane cut (`head_near`) to remove the head
while preserving the body and shadow passes; the normal near plane returns in
theater/forced camera states. Controller A/Enter traversal and menu-button ESC
are verified. Right-stick pointer selection and occasional camera jerks remain
under investigation.

## Engine constraints to respect

- The engine is documented to misbehave at high frame rates (camera shake,
  floating items), so the engine tick gets capped independently of the render
  rate. Target native 72 Hz, a 13.9 ms frame interval, then reassess from
  measured CPU and GPU timings.
- Runtime and encoding overhead need headroom on top of the engine's own frame.
