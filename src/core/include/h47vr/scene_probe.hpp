#pragma once

namespace h47vr {

// Watches the engine's scene/virtual-command entry, `EngineData.dll` RVA 0xEF60.
//
// The console `startscene` command is the only caller of `world->vtbl[0x44]`, and
// that entry is where the arguments actually land - the dispatcher throws the
// result away, so this is the only place the engine's own opinion about a scene
// request becomes observable.
//
// The detour preserves every register and tail-jumps to the trampoline, so it
// makes no assumption about the function's convention or stack cleanup.
bool SceneProbeInstall();

// Re-installs after EngineData.dll is reloaded.
bool SceneProbeRearm();

// Marks the next call as coming from the mod rather than from the engine, so
// the log can tell the two apart.
void SceneProbeMarkOwnCall();

// Calls the scene entry directly, with the mode chosen by the caller, and
// returns what it returns.
//
// This exists because the console's `startscene` command hard-codes mode 2 and
// then throws the result away, so there is no way to ask the engine "load this
// scene" and hear the answer. Going through the entry directly does both.
//
// `object` is the world object the engine passes in its own ECX, i.e.
// `*(interface + 0x59)`.
int SceneEntryInvoke(void* object, int mode, const char* first, const char* second);

}  // namespace h47vr
