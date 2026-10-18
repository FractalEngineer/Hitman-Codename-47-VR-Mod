# Graphics Bridge

The plan's first technical gate. An unproven assumption until the tests below
are run: that a 32-bit OpenGL application can share a process with the
Virtual Desktop OpenXR runtime, on the connected Quest.

## What is already proven

- The 32-bit OpenGL renderer runs on this machine, launched through Steam.
- An app-local `opengl32.dll` loads ahead of the system library and forwards
  every export to it, so the game is unaware it is being proxied.
- All GL traffic is fixed-function OpenGL 1.1 through the system ICD, so the
  proxy sits directly on the path we need to instrument.

## What is not proven

- Whether the runtime's OpenXR session can coexist in-process with the game's
  legacy WGL context. The probe reaches this step but the runtime will not
  initialise yet; see below.
- Whether eye rendering can reuse the game's GL context or needs its own.
- Whether `SwapBuffers` interception is sufficient for presentation control, or
  whether the scene draw has to be re-entered.

## First run results

`src/probe/main.cpp` builds to `h47vr-openxr-probe.exe` with the vendored loader
staged next to it, and walks the whole bridge step by step.

| Step | Result |
| --- | --- |
| Loader loads the runtime manifest | ok - `virtualdesktop-openxr-32.json` |
| 32-bit runtime DLL is genuinely i386 | ok - `machine 0x014c`, 81 exports |
| `xrEnumerateInstanceExtensionProperties` | ok - 31 extensions |
| `XR_KHR_opengl_enable` advertised | **yes** |
| `XR_KHR_D3D11_enable` advertised | yes (fallback route is available) |
| `xrCreateInstance` | FAILED, `XR_ERROR_INITIALIZATION_FAILED` (-4) |

The failure is neither architecture-specific nor related to OpenGL:

- An identical **64-bit** control build fails exactly the same way, so this is
  not the 32-bit path.
- The 32-bit failure persists with **zero** extensions enabled, so it is not the
  OpenGL binding either.

The runtime is refusing to create an instance at all, which is what Virtual
Desktop's OpenXR runtime does while no stream session is active.

What this settles:

- The plan's largest open risk is retired. Virtual Desktop really does ship an
  x86 OpenXR runtime, it advertises the OpenGL binding, and our loader reaches
  it. 32-bit was never the obstacle.
- The remaining blocker is environmental, not architectural: the Quest has to be
  connected through Virtual Desktop. Step 7, the session over the legacy
  context, has not executed yet.

Reproduce (must run outside any sandbox so it can reach the runtime):

```powershell
./tools/build.ps1 -Target h47vr_openxr_probe
./build/bin/probe/h47vr-openxr-probe.exe
```

### Side finding: the fingerprint gate proved itself on the wrong host

The probe first ran from `build/bin`, which is also where the app-local
`opengl32.dll` proxy is built. Windows loaded that proxy into the probe process,
so the mod started inside a program that is not the game. The gate refused:

```
[error] unsupported build (appid 6900 buildid 251800): 11 missing, 0 mismatched.
        No patches will be applied.
```

That is the "unknown builds receive a clear compatibility message" acceptance
test demonstrating itself on an unintended host, which is a better proof than a
deliberate test would have been. The probe now builds into `build/bin/probe` so
it exercises the system OpenGL path like a clean pre-installation run.

## Runtime facts

| Item | Finding |
| --- | --- |
| 32-bit runtime | Registered: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1` -> `virtualdesktop-openxr-32.json` |
| Runtime DLL | `C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.dll` |
| Loader | Vendored: `third_party/openxr/lib/x86/openxr_loader.dll`, built from `release-1.1.65` |
| SteamVR | 64-bit loader only, so it is not a fallback for a 32-bit host |

No `openxr_loader.dll` existed anywhere on the machine, in either architecture,
which blocked the gate before anything could be tested. `tools/fetch-openxr.ps1`
now clones the SDK at a pinned tag and builds the x86 loader with the Visual
Studio toolchain, installing it under `third_party/openxr` together with the
generated public headers and the import library.

Verified: the vendored loader is i386 (`machine 0x014c`) and exports the expected
entry points, including `xrCreateInstance`, `xrGetSystem`, `xrCreateSession`,
`xrEnumerateInstanceExtensionProperties` and `xrGetInstanceProcAddr`.

The mod ships this loader next to itself; it is the piece that reads the
registry entry above and loads the active runtime.

## Test order

1. **Loader smoke test.** Build or vendor the x86 loader, create an
   instance with the `XR_KHR_opengl_enable` extension, and enumerate system
   properties from inside a 32-bit process. This does not need the game.
2. **Coexistence.** From inside the proxied process, create a second WGL context
   on the game's window and submit a frame to OpenXR. If the runtime refuses a
   legacy WGL context, record the exact failure.
3. **Fallback.** If step 2 fails, test OpenGL-to-D3D11 GPU texture sharing and
   submit through OpenXR/D3D11.
4. **Last resort.** CPU image copies, for diagnostics only, never as the release
   architecture.

Steps 1 and 2 are cheap and decide the whole architecture, so they come before
any gameplay hook is written.

## Presentation path

`SwapBuffers` comes from `gdi32.dll`, not from `opengl32.dll`. Interception
therefore belongs on `gdi32`, either by import table patching in
`RenderOpenGL.dll` or by a hook. The proxy remains the load point for the mod;
it is not the presentation hook.

## Engine constraints to respect

- The engine is documented to misbehave at high frame rates (camera shake,
  floating items), so the engine tick gets capped independently of the render
  rate. Target native 72 Hz, a 13.9 ms frame interval, then reassess from
  measured CPU and GPU timings.
- Runtime and encoding overhead need headroom on top of the engine's own frame.
