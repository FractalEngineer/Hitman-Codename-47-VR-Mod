#pragma once

namespace h47vr {

struct ControllerInputSnapshot {
    bool valid = false;
    bool leftActive = false;
    bool rightActive = false;
    bool escapeDown = false;
    bool snapDown = false;
    bool interactDown = false;
    bool reloadDown = false;
    bool sneakDown = false;
    bool weaponModeDown = false;
    bool statusDown = false;
    bool fireDown = false;
    bool runDown = false;
    bool dropDown = false;
    float leftX = 0.0f;
    float leftY = 0.0f;
    float rightX = 0.0f;
    float rightY = 0.0f;
    unsigned long long escapeGeneration = 0;
    unsigned long long snapGeneration = 0;
};

// Publishes a controller sample. OpenXR publishes once per completed frame;
// h47vr-controller-test.txt can replace the sample for headless verification.
void ControllerInputPublish(const ControllerInputSnapshot& state);
ControllerInputSnapshot ControllerInputGet();

// Installs the GetAsyncKeyState detour used to feed the engine's existing
// keyboard state, and refreshes the optional headless test override.
bool ControllerInputStart();
void ControllerInputRearm();
void ControllerInputFrame(unsigned long frame);

}  // namespace h47vr
