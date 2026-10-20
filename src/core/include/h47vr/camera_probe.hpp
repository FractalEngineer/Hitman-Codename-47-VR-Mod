#pragma once

namespace h47vr {

// Installs the camera/world-path probes and the desktop visibility diagnostics.
// Peripheral culling is fixed by substituting the runtime eye-union FOV in the
// engine's camera projection builder (`HitmanDlc.dlc` RVA 0x92040). The later
// polygon clipper at RVA 0x93670 is not the visibility rejection point and its
// bypass remains diagnostic only.
//
// The same module samples the renderer's view object and can widen its
// horizontal FOV for tests. Ordinary desktop play keeps the native path.
bool CameraProbeInstall();

// Re-installs after HitmanDlc.dlc is reloaded.
bool CameraProbeRearm();

// Last observed camera object, or null. Meaningful once the transform has run.
void* CameraProbeObject();

// Logs the camera object's fields while <game>\h47vr-camobj.txt exists. Called
// from the presentation hook.
void CameraProbeReport(unsigned long frame);

// Drives the engine's own camera when <game>\h47vr-camwrite.txt asks for it.
//
// Two hook points, because the order of the engine's frame decides which one can
// work: the camera has to be rotated after the engine writes it and before the
// frame is built from it. `CameraProbeFrame` runs at the presentation boundary
// (the end of a frame, so it affects the next one) and `CameraProbeOnFrustum`
// runs from the proxy's glFrustum detour, inside the frame that is being built.
void CameraProbeFrame(unsigned long frame);
void CameraProbeOnFrustum();

// Diagnostic horizontal FOV from <game>\h47vr-widefov.txt, or zero when the
// desktop test gate is absent. The value is in radians.
double CameraProbeWideFovRadians();

// Publishes the symmetric horizontal FOV needed to cover the union of both
// runtime eye frusta. Zero clears the runtime request.
void CameraProbeSetRuntimeVisibilityFov(double radians);

}  // namespace h47vr
