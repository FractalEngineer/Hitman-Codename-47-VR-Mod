#pragma once

namespace h47vr {

// Intercepts the fixed-function GL matrix pipeline at the proxy boundary.
//
// The engine imports GL from the app-local opengl32.dll, and every one of that
// proxy's exports is a thunk that loads a slot and jumps to it. Replacing the
// slot therefore intercepts every call to that function without patching a
// single instruction, and the real entry point is simply the value the slot
// held before.
//
// This is the seam the VR renderer needs: the projection and the view matrix
// both pass through here, and both have to be replaced per eye. It also means
// the camera can be driven without depending on any engine address.
//
// Only the proxy sits between the engine and OpenGL, so only the proxy can
// implement this: it publishes its implementation below, and a build that is
// not the proxy (the injected module, the launcher) leaves the pointers null
// and the seam unavailable rather than half-installed.
using GlProbeInstallFn = bool (*)(const char* filter);
using GlProbeReportFn = void (*)(unsigned long);

extern GlProbeInstallFn g_gl_probe_install;
extern GlProbeReportFn g_gl_probe_report;

// Redirects the pipeline entries named by `filter` - a comma-separated list of
// GL function names, "all" for the whole set, or "none" - and returns false
// when no provider is published. Install early, before the renderer draws.
//
// The filter exists because intercepting a core GL entry is not free of
// consequences and one of these calls has to be identified as the one whose
// interception the engine objects to; narrowing the set is how that gets
// pinned down.
inline bool GlProbeInstall(const char* filter) {
    return g_gl_probe_install != nullptr && g_gl_probe_install(filter);
}

// Logs accumulated call counts and the matrices seen since the last report.
// Called from the presentation hook. No-op when no provider is published.
inline void GlProbeReport(unsigned long frame) {
    if (g_gl_probe_report != nullptr) {
        g_gl_probe_report(frame);
    }
}

}  // namespace h47vr
