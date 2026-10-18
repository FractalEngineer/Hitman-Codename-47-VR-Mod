# Hook Map

The Milestone 1 deliverable. Status as of 2026-10-18: the loading mechanism is
implemented and proven in-process, and a Ghidra pass has located the gameplay
entry points by name. No hook has been written or validated yet, and none of the
located symbols has had its semantics confirmed in a debugger.

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

## Targets

RVAs are canonical. The listed VA is only the preferred image base from the PE
headers; the modules are relocatable, so never hard-code the VA.

| # | Target | Module | Symbol | RVA | Status |
| --- | --- | --- | --- | --- | --- |
| 1 | Active camera / camera transform | `HitmanDlc.dlc` | `_ZCAMERA_MouseConCam` | `0x000264D0` | located, semantics unconfirmed |
| 1a | Camera base | `HitmanDlc.dlc` | `_ZCAMERA_BaseCamera` | `0x00020E80` | located |
| 1b | Camera control | `HitmanDlc.dlc` | `_ZCAMERA_MainCameraControl` | `0x00023940` | located |
| 1c | Follow camera | `HitmanDlc.dlc` | `_ZCAMERA_NewFollowCam2` | `0x00029700` | located |
| 1d | Scope camera | `HitmanDlc.dlc` | `_ZCAMERA_ScopeCamera` | `0x0002D060` | located (Milestone 4) |
| 1e | Camera selection list | `HitmanDlc.dlc` | `_ZLIST_SortCameraControls` | `0x00023430` | located |
| 1f | Camera class info | `HitmanDlc.dlc` | `?ZCAMERA_ClassInfo@@3VZCLASSINFOZCAMERA@@A` | `0x00244320` | located |
| 4 | Visibility / draw distance | `HitmanDlc.dlc` | `_ZGEOM_AdoptVisibility` | `0x00159050` | located |
| 4a | Visibility query | `HitmanDlc.dlc` | `_ZLIST_CheckVisible` | `0x0014DE30` | located |
| 5 | Input dispatch | `Globals.dll` | `?g_pSysInput@@3PAVZSysInput@@A` | `0x00003014` | located (singleton pointer) |
| 5a | Engine entry | `System.dll` | `RunEngine` | `0x0000A790` | located |
| 5b | Engine print | `System.dll` | `EditPrint` | `0x0000A770` | located |
| 6 | Weapon fire | `HitmanDlc.dlc` | `_ZITEM_FireShot` | `0x000EBA30` | located, semantics unconfirmed |
| 7 | Interaction / pickup | `HitmanDlc.dlc` | `_ZGEOM_BasePickupTarget` | `0x0000D980` | located |
| 7a | Interaction activate | `HitmanDlc.dlc` | `_ZGEOM_ItemActivate` | `0x00020240` | located |
| 8 | Presentation | `gdi32.dll` | `SwapBuffers` | - | import confirmed, hook site pending |

`_ZGEOM_MsgActivate` and `_ZGEOM_MsgForward` are also exported and are the other
half of the message path if the pickup route proves insufficient.

### Renderer candidates

`RenderOpenGL.dll` exports only `Create` (RVA `0x1000`), so the renderer targets
come from the Ghidra pass rather than from the export table. These are functions
that reference the leaked source paths, which makes them good candidates to
identify by disassembly next.

| Candidate | RVA | Evidence |
| --- | --- | --- |
| `FUN_0fb73680` | `0x00003680` | 20+ assertions from `RenderWintelOpenGLDraw.cpp`; likely the scene draw |
| `FUN_0fb71e90` | `0x00001E90` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb720b0` | `0x000020B0` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb721d0` | `0x000021D0` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb72ac0` | `0x00002AC0` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb73210` | `0x00003210` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb73290` | `0x00003290` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb71de0` | `0x00001DE0` | `RenderWintelOpenGLDraw.cpp` |
| `FUN_0fb83000` | `0x00013000` | `RenderWintelOpenGLTex.cpp` |
| `FUN_0fb83700` | `0x00013700` | `RenderWintelOpenGLTex.cpp` |
| `FUN_0fb84170` | `0x00014170` | `RenderWintelOpenGLTex.cpp` |
| `FUN_0fb71060` | `0x00001060` | `RenderWintelOpenGLDLL.cpp` (module init) |
| `FUN_0fb71220` | `0x00001220` | `RenderWintelOpenGLDLL.cpp` |
| `FUN_0fb713d0` | `0x000013D0` | `RenderWintelOpenGLDLL.cpp` |
| `FUN_0fb715e0` | `0x000015E0` | `RenderWintelOpenGLDLL.cpp` |
| `FUN_0fb71720` | `0x00001720` | `RenderWintelOpenGLDLL.cpp` (exported `Create` path) |
| `FUN_0fb95d50` | `0x00025D50` | `RenderBase.cpp` |
| `FUN_0fb96170` | `0x00026170` | `RenderBase.cpp` |
| `FUN_0fb97b10` | `0x00027B10` | `RenderBase.cpp` |
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
