#pragma once

namespace h47vr {

// Watches the engine's world-to-eye transform, `HitmanDlc.dlc` RVA 0x93C50
// (`FUN_0fd53c50`).
//
// That function turns a world vertex into eye space as
//
//     eye = R * (world - camPos)
//
// where `R` is a 3x3 reached through the camera object's second field and
// `camPos` is the three floats after it. The camera itself is the last stack
// argument, and the function runs many times per frame, which makes it a cheap
// and reliable place to pick the live camera pointer up.
//
// The camera object is what has to be driven. Rotating vertices at the GL
// boundary cannot work on its own: the engine has already culled against the
// frustum of the camera it believes it has, so geometry outside that frustum was
// thrown away before we ever saw it.
bool CameraProbeInstall();

// Re-installs after HitmanDlc.dlc is reloaded.
bool CameraProbeRearm();

// Last observed camera object, or null. Meaningful once the transform has run.
void* CameraProbeObject();

// Logs the camera object's fields while <game>\h47vr-camobj.txt exists. Called
// from the presentation hook.
void CameraProbeReport(unsigned long frame);

}  // namespace h47vr
