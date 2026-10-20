#include "h47vr/gl_probe.hpp"

namespace h47vr {

// Published by the opengl32 proxy, which owns the GL seam (see gl_probe.hpp).
// Null everywhere else, which is what makes the inline wrappers inert.
GlProbeInstallFn g_gl_probe_install = nullptr;
GlProbeReportFn g_gl_probe_report = nullptr;
GlStereoAvailableFn g_gl_stereo_available = nullptr;
GlStereoPerspectiveFn g_gl_stereo_perspective = nullptr;
GlStereoSourceSizeFn g_gl_stereo_source_size = nullptr;
GlPerspectiveNearFn g_gl_perspective_near = nullptr;
GlStereoBeginEyeFn g_gl_stereo_begin_eye = nullptr;
GlStereoEndEyeFn g_gl_stereo_end_eye = nullptr;

}  // namespace h47vr
