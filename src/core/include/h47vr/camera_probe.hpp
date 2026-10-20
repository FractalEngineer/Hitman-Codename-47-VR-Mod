#pragma once

namespace h47vr {

// Installs the camera/world-path probes and the desktop visibility experiment.
// The engine clips face groups inside `HitmanDlc.dlc` RVA 0x93670 during the
// `Clip` call from `CamUpdate`. The diagnostic can use that function's
// "nothing needed clipping" result, but identical-projection A/B testing showed
// no recovered geometry, so this is not a culling fix.
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

}  // namespace h47vr
