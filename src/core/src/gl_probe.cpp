#include "h47vr/gl_probe.hpp"

namespace h47vr {

// Published by the opengl32 proxy, which owns the GL seam (see gl_probe.hpp).
// Null everywhere else, which is what makes the inline wrappers inert.
GlProbeInstallFn g_gl_probe_install = nullptr;
GlProbeReportFn g_gl_probe_report = nullptr;

}  // namespace h47vr
