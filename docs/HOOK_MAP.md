# Hook Map

The Milestone 1 deliverable. Status as of 2026-10-20: loading, keyboard-free
level entry, MouseConCam readback, stereo replay and the OpenXR camera/visibility
path are implemented and live-verified. Physical testing confirmed sRGB color,
but a residual edge-culling band remains and cannot be removed by widening the
symmetric engine FOV further. The tables below retain the resolution and
verification method for each address.

This file is the single place where hook addresses and signatures are recorded.
Nothing may be hooked from an address that is not listed here with its
verification method.

## Resolution strategy

Beyond expectation, `HitmanDlc.dlc` exports **343 symbols by name**, including
every camera class, the weapon fire path, the interaction path and the visibility
helpers. `Globals.dll` exports the engine singletons and `System.dll` exports
`RunEngine` and `EditPrint`.

That makes the plan's preference - name-based lookup over frozen addresses -
achievable for most targets. The intended resolver is:

```
HMODULE gameplay = GetModuleHandleW(L"HitmanDlc.dlc");
void* camera = GetProcAddress(gameplay, "_ZCAMERA_MouseConCam");
```

The export table is still pinned: `HitmanDlc.dlc` is covered by the build
fingerprint, so if those exports ever change the mod refuses to patch. Addresses
in the tables below are recorded as RVAs for the cases where a signature is
needed anyway, notably everything inside `RenderOpenGL.dll`, which exports only
`Create`.

## Loading

| Item | Status |
| --- | --- |
| App-local `opengl32.dll` proxy | Implemented, loads in-process, 368/368 exports forwarded |
| Build fingerprint gate | Implemented; refuses to patch when any of the 11 pins differ |
| Diagnostic log | Implemented; `<game>\h47vr-logs\h47vr-<timestamp>.log`, flushed per line |
| Standalone injectable module | Implemented (`h47vr_mod.dll`) as a diagnostic path |
| Mod work thread | Runs off the loader lock, started from `DllMain` |
| Hook engine (MinHook) | Implemented, initialised in-process |
| Deferred gameplay hooking | Implemented via `LdrRegisterDllNotification`; fires on `HitmanDlc.dlc` load |
| Presentation hook | `gdi32!SwapBuffers` installed and firing; frames are counted |
| First camera hook | `_ZCAMERA_MouseConCam` installed at RVA `0x0264D0`; observed firing in a loaded level |

Measured in one run of the shipping build:

```
hooked gdi32.dll!SwapBuffers at 77815BE0
module loaded: HitmanDlc.dlc - running deferred hooks
HitmanDlc.dlc base 0E980000, camera entry rva 0x0264D0
hooked HitmanDlc.dlc!_ZCAMERA_MouseConCam at 0E9A64D0
presented frame 1 ... presented frame 5
```

Two things that run machine-check the reverse engineering:

- `0x0E9A64D0 - 0x0E980000 = 0x264D0`, the RVA recorded in the table below from
  the Ghidra pass. The module relocated: its preferred base is `0x0FCC0000`, so
  a hard-coded address would have hooked the wrong bytes. Recording RVAs was the
  right call.
- The `SwapBuffers` hook covers presentation, confirming the earlier conclusion
  that it lives in `gdi32`, not `opengl32`.

`_ZCAMERA_MouseConCam` has since been observed in a loaded level. The keyboard-free
`push:<level>` path reaches gameplay without manual input, and MouseConCam's
per-frame update at RVA `0x26F20` supplies the live pose used by stereo and
absolute aim.

The proxy never loads the system library from `DllMain`. Export thunks resolve it
on first call through `InitOnceExecuteOnce`.

## Load order

Measured in-process, and it shapes how hooks must be installed:

```
Hitman.Exe -> RenderOpenGL.dll -> opengl32.dll (our proxy, mod starts here)
                                        |
                                        +--> HitmanDlc.dlc loads later
```

At proxy load time `Globals.dll` and `System.dll` are present, but
`HitmanDlc.dlc` is **not loaded yet**. Every gameplay target in the table below
lives in that module, so gameplay hooks cannot be installed from the proxy's
startup path.

Live resolution confirms the strategy and the addresses:

```
Globals.dll   ?g_pSysInput@@3PAVZSysInput@@A  -> 0FFF3014  rva 0x003014
System.dll    RunEngine                       -> 0FFAA790  rva 0x00A790
HitmanDlc.dlc (all gameplay probes)           -> not loaded yet
```

The intended fix is to be notified rather than to poll: `LdrRegisterDllNotification`
gives a callback when `HitmanDlc.dlc` (and `RenderOpenGL.dll`) load, which is
where the deferred hook installation belongs. Renderer work can stay on the
proxy's own startup path because `RenderOpenGL.dll` is already there.

## Console command dispatcher

The engine's console and config lines both end up in one dispatcher in
`System.dll`, so the mod can enter a level without synthesising keystrokes at
all: it submits the line to that dispatcher itself.

ABI, read from the machine code rather than guessed from the decompiler:

```
call site  System.dll rva 0x104AD   mov ecx, edi / push name / push arg / call 0xE590
entry      System.dll rva 0xE590    push esi / mov esi, [esp+8] ... ret 8
```

`ret 8` with the object in `ECX` is `__thiscall`: two stack arguments, callee
cleanup. The two arguments are the command name and its argument, already split
by the caller at the first `=` or space:

| Item | Value |
| --- | --- |
| `System.dll` dispatcher | RVA `0x0000E590`, `int __thiscall(void* self, const char* name, const char* arg)` |
| Its only caller (line splitter) | `System.dll` RVA `0x00104AD` |
| `self` | `Globals.dll` `?g_pSysInterface@@3PAVZSysInterface@@A`, RVA `0x00003010` |
| `startscene` handler | `this+0x59` -> vtable `+0x44` called with `(2, sceneName, 0)` |

Because MSVC rejects `__thiscall` on free functions, the mod reaches this
through two naked thunks in `src/core/src/console_driver.cpp`: one detour that
preserves every register and tail-jumps to the trampoline, and one invoke thunk
that loads `ECX` and jumps to the trampoline. Both let the original `ret 8` do
the cleanup.

The line the mod submits comes from `<game>\h47vr-scene.txt` (first non-empty
line) or the `H47VR_SCENE` environment variable; with neither present the
driver is idle. `tools/iterate.ps1 -Scene C0_1` writes that file and reports
`objectDrawA` from the run.

## Targets

RVAs are canonical. The listed VA is only the preferred image base from the PE
headers; the modules are relocatable, so never hard-code the VA.

| # | Target | Module | Symbol | RVA | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | Active camera / camera transform | `HitmanDlc.dlc` | `_ZCAMERA_MouseConCam` | `0x000264D0` | native yaw update hooked and verified |
| 1a | Camera base | `HitmanDlc.dlc` | `_ZCAMERA_BaseCamera` | `0x00020E80` | located |
| 1b | Camera control | `HitmanDlc.dlc` | `_ZCAMERA_MainCameraControl` | `0x00023940` | located |
| 1c | Follow camera | `HitmanDlc.dlc` | `_ZCAMERA_NewFollowCam2` | `0x00029700` | located |
| 1d | Scope camera | `HitmanDlc.dlc` | `_ZCAMERA_ScopeCamera` | `0x0002D060` | located (Milestone 4) |
| 1e | Camera selection list | `HitmanDlc.dlc` | `_ZLIST_SortCameraControls` | `0x00023430` | located |
| 1f | Camera class info | `HitmanDlc.dlc` | `?ZCAMERA_ClassInfo@@3VZCLASSINFOZCAMERA@@A` | `0x00244320` | located |
| 1g | MouseConCam per-frame update / live pose | `HitmanDlc.dlc` | (not exported) | `0x00026F20` | hooked; yaw/pitch read from `+0x2D/+0x31/+0x35`; parent camera `self+0x08`, position `camera+0x08` |
| 1h | MouseConCam follow-distance targets | `HitmanDlc.dlc` | (not exported) | `0x0002714F` / `0x00027158` | gated first-person override; immediates `150.0` / `100.0` |
| 4 | Visibility / draw distance | `HitmanDlc.dlc` | `_ZGEOM_AdoptVisibility` | `0x00159050` | descriptor initializer only; helper RVA `0x00159080` is a generic message handler |
| 4a | Visibility query | `HitmanDlc.dlc` | `_ZLIST_CheckVisible` | `0x0014DE30` | descriptor initializer only; helper RVA `0x0014DE60` is a generic message handler |
| 5 | Input dispatch | `Globals.dll` | `?g_pSysInput@@3PAVZSysInput@@A` | `0x00003014` | located (singleton pointer) |
| 5a | Engine entry | `System.dll` | `RunEngine` | `0x0000A790` | located |
| 5b | Engine print | `System.dll` | `EditPrint` | `0x0000A770` | located |
| 5c | Console / config command dispatcher | `System.dll` | (not exported) | `0x0000E590` | hooked, ABI confirmed from machine code |
| 5d | Interface singleton for the dispatcher | `Globals.dll` | `?g_pSysInterface@@3PAVZSysInterface@@A` | `0x00003010` | located (singleton pointer) |
| 5e | Native look accumulator | `Globals.dll` -> interface | `g_pSysInterface + 0xA95/+0xA99` | - | direct OpenXR yaw feed verified |
| 5f | Per-key input state records | `Globals.dll` -> interface | `+0xAB6` edge / `+0xAB7` down, stride 15 | - | controller movement/menu bridge |
| 6 | Weapon fire | `HitmanDlc.dlc` | `_ZITEM_FireShot` | `0x000EBA30` | located, semantics unconfirmed |
| 7 | Interaction / pickup | `HitmanDlc.dlc` | `_ZGEOM_BasePickupTarget` | `0x0000D980` | located |
| 7a | Interaction activate | `HitmanDlc.dlc` | `_ZGEOM_ItemActivate` | `0x00020240` | located |
| 8 | Presentation | `gdi32.dll` | `SwapBuffers` | - | import confirmed, hook site pending |

`_ZGEOM_MsgActivate` and `_ZGEOM_MsgForward` are also exported and are the other
half of the message path if the pickup route proves insufficient.

### Native camera orientation

`MouseConCam`'s per-frame update at RVA `0x26F20` rotates the camera offset
vector at `+0x2D/+0x31/+0x35` through the native rotation helper at RVA
`0x26300`. The input it consumes is the accumulated look delta at
`g_pSysInterface + 0xA95/+0xA99`. The mod writes headset yaw directly into that
accumulator immediately before calling the original update; no window message,
cursor position or render-size assumption is involved. A synthetic 30-degree
head pose changed the live readback from 0 to 30.0373 degrees with zero mouse
messages posted.

The older `WM_MOUSEMOVE` path remains only for manual `h47vr-look.txt` requests
and migration fallback. The camera still performs its own translation,
collision and visibility work.

After the original update returns, the hook resolves `MouseConCam+0x08` to the
parent camera object and adjusts its world position at `camera+0x08`.
`camera_lower` changes this real float Y value, and the offset vector's vertical
pitch component is removed there so native pitch is available to the visibility
pass without moving the eye up or down along the follow sphere. This replaced
the older GL-seam vertical shift.

### OpenXR controller mapping

The live Quest profile uses `units_per_m=110` provisionally and 180-degree/second
smooth turning. Left-stick axes are inverted from the initial mapping because
the engine's forward/left state is opposite OpenXR's initial signs. Movement and
menu directions post held `WM_KEYDOWN`/`WM_KEYUP` messages; face buttons are
100 ms taps. The current mapping is:

| OpenXR input | Engine action |
| --- | --- |
| left thumbstick | W/S/A/D movement, with corrected signs |
| left thumbstick click | Run (left Shift) |
| right thumbstick | Native smooth yaw |
| right thumbstick click | Directional 30-degree snap when deflected |
| left menu | ESC/back |
| left/right trigger | Fire (left mouse button) |
| right A | Interact; click the native menu pointer |
| right B | Reload; Escape in menus |
| left X | Toggle weapon mode |
| left Y | Status |
| right grip | Sneak toggle |
| left grip | Drop held item |

The ESC startup guard ignores a button already held during the first four
seconds of launch. This prevents the OpenXR session-entry press from closing the
intro. In non-gameplay screens the right stick posts absolute `WM_MOUSEMOVE`
coordinates and A or either trigger posts an 80 ms held left click; left-stick
arrows remain a fallback. Full physical menu traversal still needs a check.

### Renderer candidates

`RenderOpenGL.dll` exports only `Create` (RVA `0x1000`), so the renderer targets
come from analysis rather than the export table. Rather than guess from function
sizes, `tools/ghidra/DumpReferences.java` follows the imported GL entry points
and reports which internal function calls each one. That is what dated the map
below, and `src/core/src/renderer_probe.cpp` then confirmed the rates at run
time by counting calls with convention-agnostic detours.

| Target | RVA | Why it matters | Measured |
| --- | --- | --- | --- |
| `FUN_0fb748f0` | `0x0048F0` | presentation only: calls `glFinish` and `SwapBuffers` | ~1 per frame (118 in 120) |
| `FUN_0fb73680` | `0x003680` | face-list draw consumer, not the gameplay update | ~21 per frame; **hooked and replayed per perspective eye** |
| `FUN_0fb98a40` | `0x028A40` | renderer view setup; first stack argument owns projection constants | horizontal FOV at `+0x16E`, viewport `+0x156/+0x15E` |
| `FUN_0fb76590` | `0x006590` | per-object draw: `glPushMatrix`, `glMultMatrixf`, `glTranslatef`, `glScalef`, `glBindTexture`, `glVertexPointer`, `glDrawElements` | 0 at the front end - world geometry is dormant |
| `FUN_0fb76270` | `0x006270` | second object path: matrix stack, `glCullFace` | 0 at the front end |
| `FUN_0fb74930` | `0x004930` | 2D pass: `glOrtho` + `glViewport` + `glBegin`/`glEnd` | menu/UI layer |
| `FUN_0fb75300` | `0x005300` | 2D pass, same shape | menu/UI layer |
| `FUN_0fb75a10` | `0x005A10` | 2D pass, same shape | menu/UI layer |
| `FUN_0fb75f20` | `0x005F20` | 2D pass, same shape | menu/UI layer |
| `FUN_0fb72ec0` | `0x002EC0` | `glOrtho` + `glLoadIdentity`, `glEnable`/`glDisable` | another 2D/ortho setup |
| `FUN_0fb715e0` | `0x0015E0` | texture creation: `glGenTextures`, `glTexImage2D` | called once per texture |
| `FUN_0fb83000` | `0x013000` | texture upload, `glTexImage2D` x5 | |
| `FUN_0fb83700` | `0x013700` | texture upload | |
| `FUN_0fb84170` | `0x014170` | texture upload | |

Two consequences for Milestone 2, both from measurement rather than inference:

- `0x48F0` is the presentation boundary, not the draw loop. The eye loop belongs
  at `0x3680`, which receives already-built face lists; replaying it does not
  run gameplay, audio or scripts again.
- Perspective calls through `0x3680` are rendered once per eye into half
  viewports. Orthographic calls remain single. The current fixed-IPD prototype
  scales X projection by 0.5 and shifts eye-space vertices by `+/- IPD/2`.
- The fixed-function projection is intercepted in the proxy's GL seam. Runtime
  views now replace that scale with a per-eye `glFrustum` built from OpenXR's
  four FOV tangents and the engine's current near/far planes.
- The engine rebuilds its gameplay camera projection and visibility state in
  `HitmanDlc.dlc` `FUN_0fd52040` (`0x92040`). Its first stack argument is the
  horizontal FOV. The mod hooks that function and substitutes the symmetric
  horizontal FOV covering the union of both OpenXR eye frusta while gameplay is
  active. This keeps the renderer projection, face-list rejection and clipping
  in one engine-owned camera state.
- That engine-FOV substitution accepted the runtime union in the physical
  headset and produced the last good culling result. An extra 6-degree-per-side
  margin was accepted too but did not change the visible residual edge; the
  margin control has been removed. The XR eye image can now be mirrored to the
  desktop, and that reproduction shows the remaining band becomes a half-frame
  loss under 45-degree pitch. Future work should fix orientation-dependent
  visibility rather than widening one symmetric angle.
- The geometry `+0x398` path called by `CamUpdate` resolves to
  `HitmanDlc.dlc` RVA `0x95AE0`; its post-call probe is gated by
  `h47vr-camobj.txt`. Forcing the shared bounds predicate at `0xA1B60` did not
  recover peripheral geometry. The downstream face-list clipper at `0x93670`
  and the `h47vr-clipall` toggle were also ruled out under an identical wide
  projection.

`RenderOpenGL.dll` loaded at its preferred base `0x0FB70000` in that run, while
`HitmanDlc.dlc` relocated. Both cases occur, which is why every address in this
file is an RVA.
| `FUN_0fb98530` | `0x00028530` | `RenderWintel.cpp` |
| `FUN_0fb9b8d0` | `0x0002B8D0` | `RenderWintel.cpp` |

The full per-module anchor dumps, including every cross-reference and the RTTI
descriptors, are reproduced with `tools/ghidra-analyze.ps1` into
`tools/_ghidra_out/` (git-ignored). The export tables are committed under
`docs/re/`.

## Reverse engineering anchors

From embedded `Z:\` build paths and RTTI names, before any decompilation:

- Camera: `Z:\Hitman\Camera\Source\MouseConCam.cpp`,
  `_ZCAMERA_MouseConCam`, `_ZCAMERA_MainCameraControl`, `_ZCAMERA_BaseCamera`,
  `_ZCAMERA_NewFollowCam2`, plus `CSniperCamera` and `CBinocularCamera`.
- The exported `?ZCAMERA_ClassInfo@@3VZCLASSINFOZCAMERA@@A` object lets the
  active camera be found by class name at run time instead of a frozen address.
  Prefer that over hard-coded addresses.
- Weapon: `_ZITEM_FireShot`, `ZItemGroupWeapon`, `ZItemAmmo`.
- Interaction: `Z:\Hitman\Bkgnd\Source\ZItemActivate.cpp`, `BasePickupTarget`.
  Interactions must route through this path so distance checks, inventory, AI
  reactions and mission scripts stay authoritative.
- Other confirmed subsystem directories: `Z:\Hitman\Laptop\Source`,
  `Z:\Hitman\CUI\Source`, `Z:\Hitman\Enemies\Source`,
  `Z:\Engine\EventsBase\Source`, `Z:\Engine\GeomsBase\Source`,
  `Z:\Engine\GeomsExtend\Source`, `Z:\Engine\Dynamics\Source`,
  `Z:\Engine\ZStdLib\Source`.

## Facts established this milestone

- `RenderOpenGL.dll` imports 67 functions from `opengl32.dll`, and 9 from
  `gdi32.dll`: `SwapBuffers`, `SelectPalette`, `DescribePixelFormat`,
  `GetDeviceCaps`, `SetPixelFormat`, `GetStockObject`, `RealizePalette`,
  `CreatePalette`, `GetPixelFormat`.
- `gdi32`'s pixel format functions delegate to `opengl32!wgl*` at run time. Any
  proxy in front of `opengl32` must re-export them or the game fails at pixel
  format selection. This is why the proxy shadows all 368 exports.
- `RenderOpenGL.dll` is fixed-function OpenGL 1.1: `glFrustum`, `glMultMatrixf`,
  `glTranslatef`, `glViewport`, `glPushMatrix`, client-side vertex arrays via
  `glDrawElements`, textures via `glTexImage2D`/`glTexSubImage2D`, presentation
  via `SwapBuffers`. No shaders, VBOs or FBOs. The full 67-symbol import list is
  in `src/proxy_opengl32/opengl32.def` for the 67-function case and in the
  generated file for the full surface.
- `Hitman.Exe` is a thin host that imports only kernel32 and contains the
  obfuscated Steam presence check.
- `Globals.dll` is the export hub the other gameplay modules link against; it
  imports nothing itself. `System.dll` imports `globals.dll`, `gdi32.dll`,
  `user32.dll` and `advapi32.dll`. `HitmanDlc.dlc` imports `globals.dll`,
  `xmlparse.dll` and `gsc.dll`.
- `ar405.exe` is a bundled Adobe Acrobat Reader 4.05 for the manual PDF, not the
  game.
- `HitmanDlc.dlc` exports 343 named symbols. Beyond the camera and weapon
  entries listed above, that includes 14 `_ZCAMERA_*` classes, the `_ZGEOM_*`
  action and message handlers, `_ZLIST_*` notifiers, `_ZITEMGROUPWEAPON_*`,
  `_ZDYNAMICS2_*` physics entry points and ~70 `_ZWINGROUP_*` / `_ZWINOBJ_*`
  menu and HUD groups that Milestone 4 will need.
- `Globals.dll` exports exactly six symbols: the engine version and the
  `g_pSysCom`, `g_pSysFile`, `g_pSysInput`, `g_pSysInterface` and `g_pSysMem`
  singletons. `g_pSysInput` is a pointer variable, so reading it yields the live
  input object.
- `System.dll` exports `RunEngine` (RVA `0xA790`) and `EditPrint` (`0xA770`).
- `RenderOpenGL.dll` exports only `Create` (RVA `0x1000`), so renderer work needs
  RVAs plus signatures rather than names.
- The camera classes are also registered through a contiguous table of
  `ZCLASSINFO` / name-string pairs spanning RVA `0x0021B8E0`-`0x0021B914`, with
  the mangled `?ZCAMERA_ClassInfo@@...` name string at RVA `0x0021C278`. That
  table is the authoritative list of which camera classes exist, and a good
  cross-check for anything resolved by name.
- Preferred image bases differ per module, which is why RVAs are recorded:
  `RenderOpenGL.dll` `0x0FB70000`, `System.dll` `0x0FFA0000`,
  `Globals.dll` `0x0FFF0000`, `HitmanDlc.dlc` `0x0FCC0000`.

## Method for each hook

1. Resolve by name where the module exports the symbol. No address is needed and
   the build pin already guarantees the table has not changed.
2. Where only an address exists, confirm the Ghidra interpretation live with
   x32dbg: arguments, calling convention, and the observed effect of the call.
3. Record the RVA here together with a byte signature that is validated before
   patching, so a different build can never be patched by accident.
4. Only then write the hook, behind the fingerprint gate.

Nothing in this file is trusted until step 2 has been done for that symbol. The
Ghidra pass produced candidates, not conclusions.
