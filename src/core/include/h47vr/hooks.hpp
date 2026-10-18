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

// Same, but fires on every load of the module. Level transitions reload the
// engine's modules, which throws away every patch, so the hooks have to be
// re-armed rather than installed once.
bool WatchModuleLoads(const wchar_t* moduleName, void (*callback)());

// Writes out the load/unload notifications seen so far, and clears them.
//
// They are recorded rather than logged when they arrive because the
// notification runs under the loader lock, where taking the logger's mutex can
// deadlock against a thread that holds it and is waiting for the loader.
void HooksFlushModuleEvents();

// Stops watching, without shutting the hook engine down.
void StopModuleWatch();

}  // namespace h47vr
