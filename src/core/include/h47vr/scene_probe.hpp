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

}  // namespace h47vr
