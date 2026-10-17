# Hook Map

The Milestone 1 deliverable. Status as of 2026-10-18: the loading mechanism is
implemented and proven in-process; every gameplay hook is still a target with no
validated address, because Ghidra has not yet been run against the binaries.

This file is the single place where hook addresses and signatures are recorded.
Nothing may be hooked from an address that is not listed here with its
verification method.

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

## Targets

Named for the subsystem each hook has to reach. Addresses are filled in as they
are identified and confirmed.

| # | Target | Module | Address | Status |
| --- | --- | --- | --- | --- |
| 1 | Active camera / camera transform | `Hitmandlc.dlc` | - | target identified, not located |
| 2 | Projection and view matrix setup | `RenderOpenGL.dll` | - | target identified, not located |
| 3 | World draw entry (per-frame scene draw) | `RenderOpenGL.dll` | - | target identified, not located |
| 4 | Visibility / draw-distance calculation | `Hitmandlc.dlc` | - | target identified, not located |
| 5 | Input dispatch (SCode action map) | `System.dll` / `Globals.dll` | - | target identified, not located |
| 6 | Weapon fire (`_ZITEM_FireShot`) | `Hitmandlc.dlc` | - | anchor only |
| 7 | Interaction / pickup (`ZItemActivate`) | `Hitmandlc.dlc` | - | anchor only |
| 8 | Presentation (`SwapBuffers`) | `gdi32.dll` | - | import confirmed, hook site pending |

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

## Method for each hook

1. Locate the function in Ghidra (headless, with the x86 PE loaded).
2. Confirm the interpretation live: arguments, calling convention, and the
   observed effect of calling it.
3. Record the address here together with a byte signature that is validated
   before patching, so a different build can never be patched by accident.
4. Only then write the hook, behind the fingerprint gate.

The plan's rule stands: prefer name-based lookup (RTTI class info, exported
symbols) over frozen addresses wherever one exists.
