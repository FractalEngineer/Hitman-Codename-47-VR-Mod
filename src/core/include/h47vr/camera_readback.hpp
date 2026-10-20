#pragma once

namespace h47vr {

struct CameraReadback {
    bool valid = false;
    float yaw = 0.0f;
    float pitch = 0.0f;
    float offset[3] = {};
    float anchor[3] = {};
    float cameraPosition[3] = {};
    void* object = nullptr;
    unsigned long updates = 0;
    unsigned long epoch = 0;
    unsigned long long lastUpdateMs = 0;
};

// Hooks MouseConCam's per-frame update and publishes the orientation the
// engine just calculated. This is diagnostic data: the look driver remains the
// only path that changes the engine's camera.
bool CameraReadbackInstall();
bool CameraReadbackRearm();

// Latest orientation, safe to call from the render thread.
bool CameraReadbackGet(CameraReadback* state);

// True only while MouseConCam has updated recently. Menus and scripted camera
// states leave the last pose readable but must not be treated as gameplay.
bool CameraReadbackGameplayActive();

// Publishes <game>\h47vr-camread.txt at the presentation boundary.
void CameraReadbackFrame(unsigned long frame);

}  // namespace h47vr
