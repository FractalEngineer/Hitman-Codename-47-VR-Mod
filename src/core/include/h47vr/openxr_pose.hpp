#pragma once

#include <windows.h>

namespace h47vr {

struct OpenXrEyePose {
    bool valid = false;
    bool fovValid = false;
    float eyeXMetres[2] = {};
    float fov[2][4] = {};
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    unsigned long long frame = 0;
};

bool OpenXrPoseStart();

// Gated by <game>\h47vr-openxr.txt. Initialisation happens on the render thread
// while the game's WGL context is current.
void OpenXrPoseFrame(HDC device, unsigned long frame);

// Latest predicted eye poses. The fixed-IPD path remains active until this
// returns true.
bool OpenXrPoseGet(OpenXrEyePose* pose);

}  // namespace h47vr
