# Third-party dependencies

## openxr

The 32-bit OpenXR loader the mod ships with, plus the public headers it was
built against.

| Item | Value |
| --- | --- |
| Source | `KhronosGroup/OpenXR-SDK-Source` tag `release-1.1.65` |
| Licence | Apache-2.0 (`third_party/openxr/LICENSE`) |
| Built by | `tools/fetch-openxr.ps1` (VS x86 toolchain, `DYNAMIC_LOADER=ON`) |
| Artifacts | `lib/x86/openxr_loader.dll`, `lib/x86/openxr_loader.lib`, `include/openxr/*.h` |

Why it is vendored: no `openxr_loader.dll` exists on the target machine in any
architecture. The loader is what the application links against and what reads
`HKLM\SOFTWARE\WOW6432Node\Khronos\OpenXR\1\ActiveRuntime` to find the runtime,
so it has to travel with the mod.

The SDK source tree itself is cloned into `third_party/openxr-sdk` by the fetch
script and is git-ignored. Only the built artifacts, headers and licence are
committed, so the repository stays small while the dependency stays reproducible.

Rebuild with:

```powershell
./tools/fetch-openxr.ps1 -Force
```

## Not vendored

- **MinHook** (x86 function hooks) - still to be added.
- **Emulation/analysis tools** (Ghidra, x32dbg) are installed locally, not part
  of the repository.
