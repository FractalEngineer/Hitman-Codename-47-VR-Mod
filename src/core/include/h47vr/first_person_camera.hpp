#pragma once

namespace h47vr {

// Experimental gameplay-camera override. MouseConCam normally drives its
// follow distance toward 150 game units (100 while leaning). The supported
// build has two fixed immediates for those targets; while the gate file is
// present this module replaces both with the configured distance so the engine
// performs its own transform, visibility and collision work from the head.
//
// Off by default. This is deliberately separate from head tracking and stereo:
// it changes the gameplay camera, not just how that camera is rendered.
bool FirstPersonCameraStart();
void FirstPersonCameraFrame(unsigned long frame);
void FirstPersonCameraRearm();
bool FirstPersonCameraActive();

}  // namespace h47vr
