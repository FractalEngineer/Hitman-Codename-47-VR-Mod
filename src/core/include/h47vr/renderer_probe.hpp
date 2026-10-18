#pragma once

namespace h47vr {

// Hooks the renderer candidates identified by the Ghidra reference dump and
// counts how often each is called.
//
// The detours are naked thunks that bump a counter and jump to the original
// code without touching the stack, so they are safe regardless of the target's
// calling convention. That matters: the renderer's internal functions are C++
// and are likely __thiscall, which cleans its own arguments.
//
// Install is deferred to the first presented frame, by which point the renderer
// is fully loaded, relocated and running.
bool RendererProbeInstall();

// Logs the accumulated call counts. Called from the presentation hook.
void RendererProbeReport(unsigned long frame);

// Allows the probe to be installed again, for when the renderer is reloaded
// and the fresh mapping no longer carries the detour.
void RendererProbeReset();

// Logs the renderer's current base and whether our detour is still in place,
// re-installing it when the module was reloaded underneath us. Called at the
// moments that matter rather than every frame.
void RendererProbeReportState(const char* when);

}  // namespace h47vr
