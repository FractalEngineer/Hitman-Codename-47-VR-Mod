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
    float rollDegrees = 0.0f;
    unsigned long long frame = 0;
};

struct OpenXrStereoTarget {
    unsigned int framebuffer[2] = {};
    int width = 0;
    int height = 0;
};

bool OpenXrPoseStart();

// Gated by <game>\h47vr-openxr.txt. Initialisation happens on the render thread
// while the game's WGL context is current.
void OpenXrPoseFrame(HDC device, unsigned long frame);

// Latest predicted eye poses. The fixed-IPD path remains active until this
// returns true.
bool OpenXrPoseGet(OpenXrEyePose* pose);

// Starts one OpenXR frame before the first stereo draw and returns the acquired
// per-eye render targets. Called on the render thread. The frame is submitted
// from OpenXrPoseFrame at the presentation boundary.
bool OpenXrBeginStereoTarget(OpenXrStereoTarget* target);

// Drops acquired direct-render images while preserving the open XR frame so the
// desktop side-by-side fallback can still be copied and submitted.
void OpenXrCancelStereoTarget();

bool OpenXrDirectStereoActive();

}  // namespace h47vr
