# Hitman: Codename 47 VR Mod

A native 32-bit x86 mod that adds PC VR support to the Steam release of
*Hitman: Codename 47* (appid 6900).

The plan of record is [PLAN.md](PLAN.md). It supersedes the earlier
chat-transcript copy kept as `PLAN.MD.txt`.

## Status

[PLAN.md](PLAN.md) carries the living progress checklist; it is updated in the
same commit as the work it describes, so it is always the current picture.

Milestone 1 (baseline and integration points) is complete. Milestone 2 is in
progress: the engine camera pose is read back live, absolute look is closed-loop,
and a 67 mm side-by-side stereo prototype renders both eyes from one gameplay
update. A gated OpenXR backend now feeds runtime eye positions, asymmetric FOV
tangents and predicted head orientation into that path. Gameplay now renders
directly into depth-complete OpenXR eye swapchains under the SteamVR null HMD;
physical Quest testing has validated the 67 mm IPD, head tracking and theater
placement. The narrow engine polygon clipper that removed peripheral geometry is
identified and bypassed during immersive gameplay; a desktop 122-degree A/B test
exposed substantially more world geometry and restored cleanly. Physical-headset
confirmation of the culling fix remains. Menus and cinematics use theater mode,
with immersive stereo reserved for gameplay. Gameplay can now move
from the 150-unit follow camera into a first-person position through the
engine's own MouseConCam transform; head hiding and final eye-height calibration
remain. The desktop presentation path now switches between one-pass theater mode
and gameplay stereo from live MouseConCam freshness. The OpenXR backend now
submits theater quads and native per-eye gameplay projection layers through real
GL swapchains.

Milestone 1 established:

- The install matches every supported-build pin, and the mod refuses to patch
  anything else.
- The app-local `opengl32.dll` proxy loads into the running game, verifies the
  build, writes diagnostics, and forwards all 368 system exports.
- Both renderers were validated; the OpenGL path now runs with the proxy in
  place.

See [docs/BASELINE.md](docs/BASELINE.md) for verified environment facts,
[docs/HOOK_MAP.md](docs/HOOK_MAP.md) for reverse-engineering progress, and
[docs/GRAPHICS_BRIDGE.md](docs/GRAPHICS_BRIDGE.md) for the next gate.

## Layout

| Path | Purpose |
| --- | --- |
| `pins/` | Supported-build fingerprints. The mod refuses to patch anything else. |
| `tools/` | Build, install, uninstall and diagnostic scripts. |
| `src/core/` | Shared infrastructure: logging, fingerprints, path and module helpers. |
| `src/proxy_opengl32/` | App-local `opengl32.dll` proxy and its generated thunks. |
| `src/mod/` | Injectable copy of the mod, for the diagnostic launcher. |
| `src/launcher/` | Development launcher: verify, run, inject. |
| `docs/` | Baseline, hook map, graphics bridge. |

## Quick start

```powershell
# Verify the installed build matches the supported pins.
./tools/fingerprint.ps1

# Build, install, launch through Steam and print the proxy log.
./tools/iterate.ps1

# Remove the mod and prove the install is untouched.
./tools/uninstall-mod.ps1
```

Steps individually, when you need them:
`./tools/build.ps1`, `./tools/install-mod.ps1`, `./tools/uninstall-mod.ps1`.

## Launch requirements

`Hitman.Exe` contains a Steam presence check with obfuscated strings. Started
directly it shows `Failed to find Steam.` and exits with `0xFFFFFFFF`, so the game
must be launched through the Steam client. `runme.exe` in the install is Valve's
DRM launcher, and `runme.dat` names `hitman.exe` as its target.

Because of that check, an out-of-tree copy of the game cannot be launched. Mod
testing installs into the real game directory and uninstalls afterwards;
`tools/uninstall-mod.ps1` re-verifies the build pins to prove the copy is intact.

## Non-goals

A full decompilation. A replacement engine. Standalone Quest builds.
