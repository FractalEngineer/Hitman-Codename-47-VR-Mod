#pragma once

#include <windows.h>

namespace h47vr {

// Starts the hook engine. Safe to call more than once; returns false if it
// could not be initialised.
bool HooksInitialize();
void HooksShutdown();

// Detours an exported symbol of an already-loaded module. Returns false and
// leaves the target untouched when the module or the symbol is missing.
bool HookSymbol(const wchar_t* moduleName, const char* symbol, void* detour, void** original);

// Detours an absolute address.
bool HookAddress(void* target, void* detour, void** original);

// Invokes a callback the first time the named module loads.
//
// This matters because the gameplay module is not present when the proxy
// starts: opengl32 loads alongside the renderer, and HitmanDlc.dlc arrives
// later. The callback runs on a worker thread, never under the loader lock.
bool WatchModuleLoad(const wchar_t* moduleName, void (*callback)());

// Stops watching, without shutting the hook engine down.
void StopModuleWatch();

}  // namespace h47vr
