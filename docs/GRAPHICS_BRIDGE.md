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
  legacy WGL context.
- Whether eye rendering can reuse the game's GL context or needs its own.
- Whether `SwapBuffers` interception is sufficient for presentation control, or
  whether the scene draw has to be re-entered.

## Runtime facts

| Item | Finding |
| --- | --- |
| 32-bit runtime | Registered: `HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1` -> `virtualdesktop-openxr-32.json` |
| Runtime DLL | `C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr-32.dll` |
| Loader | **Not installed.** No `openxr_loader.dll` in `System32` or `SysWOW64` |
| SteamVR | 64-bit loader only, so it is not a fallback for a 32-bit host |

The missing loader is the first practical obstacle: any test has to vendor or
build the x86 `openxr_loader` and place it next to the mod, because the loader is
what an application links against and it is what locates the active runtime.

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
