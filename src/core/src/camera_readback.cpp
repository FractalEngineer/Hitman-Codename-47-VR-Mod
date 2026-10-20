#include "h47vr/camera_readback.hpp"

#include "h47vr/first_person_camera.hpp"
#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/look_driver.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/stereo_renderer.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

namespace h47vr {
namespace {

// MouseConCam's per-frame update. The camera's player-relative offset is held
// at +0x2D/+0x31/+0x35 and is rotated by the engine from the mouse input before
// this function builds the view basis. Reading it after the update therefore
// gives the engine's current orientation instead of our dead-reckoned pixel
// integral.
constexpr unsigned long kMouseConCamUpdateRva = 0x00026F20;
constexpr unsigned long kResolveHandleRva = 0x000262D0;
constexpr int kScanBytes = 0x93;

using MouseConCamUpdateFn = void(__fastcall*)(void* self);
MouseConCamUpdateFn g_real_update = nullptr;

std::atomic<bool> g_installed{false};
std::mutex g_state_mutex;
CameraReadback g_state;
void* g_corrected_camera_object = nullptr;
float g_applied_camera_correction[3] = {};

bool Readable(const void* address, size_t bytes) {
    if (address == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & kReadable) == 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= end;
}

float Degrees(float radians) {
    return radians * 57.29577951308232f;
}

float* CameraPosition(void* self) {
    if (!Readable(self, 0x0C)) {
        return nullptr;
    }
    auto* parent = *reinterpret_cast<unsigned char**>(
        static_cast<unsigned char*>(self) + 0x08);
    if (!Readable(parent, 0x14)) {
        return nullptr;
    }
    return reinterpret_cast<float*>(parent + 0x08);
}

bool CameraAnchor(void* self, float anchor[3]) {
    if (anchor == nullptr || !Readable(self, 0x2D)) {
        return false;
    }
    auto* control = *reinterpret_cast<unsigned char**>(
        static_cast<unsigned char*>(self) + 0x29);
    if (!Readable(control, 0x10)) {
        return false;
    }
    const unsigned int handle =
        *reinterpret_cast<unsigned int*>(control + 0x0C);
    const HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    if (gameplay == nullptr) {
        return false;
    }
    using ResolveHandleFn = void* (__fastcall*)(unsigned int);
    const auto resolve = reinterpret_cast<ResolveHandleFn>(
        reinterpret_cast<unsigned char*>(const_cast<HMODULE>(gameplay)) +
        kResolveHandleRva);
    void* target = resolve(handle);
    if (target == nullptr) {
        return false;
    }
    void** vtable = *reinterpret_cast<void***>(self);
    if (vtable == nullptr) {
        return false;
    }
    void* getAnchor = vtable[0x5C / 4];
    if (getAnchor == nullptr) {
        return false;
    }
    using GetAnchorFn = void(__thiscall*)(void*, float*, void*);
    reinterpret_cast<GetAnchorFn>(getAnchor)(self, anchor, target);
    return std::isfinite(anchor[0]) && std::isfinite(anchor[1]) &&
           std::isfinite(anchor[2]);
}

void RestoreCameraCorrection(void* self) {
    if (self == nullptr || self != g_corrected_camera_object) {
        return;
    }
    float* position = CameraPosition(self);
    if (position != nullptr) {
        for (int axis = 0; axis < 3; ++axis) {
            position[axis] += g_applied_camera_correction[axis];
            g_applied_camera_correction[axis] = 0.0f;
        }
    }
}

void ApplyCameraCorrection(void* self, float lowerUnits, float forwardUnits) {
    float* position = CameraPosition(self);
    if (position == nullptr || !std::isfinite(lowerUnits) ||
        !std::isfinite(forwardUnits) ||
        !Readable(self, 0x59)) {
        g_corrected_camera_object = nullptr;
        for (float& value : g_applied_camera_correction) {
            value = 0.0f;
        }
        return;
    }
    const auto* offset = reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(self) + 0x2d);
    const float horizontal = std::sqrt(offset[0] * offset[0] +
                                       offset[2] * offset[2]);
    const float radius = std::sqrt(offset[0] * offset[0] +
                                   offset[1] * offset[1] +
                                   offset[2] * offset[2]);

    // MouseConCam stores the follow offset after applying both yaw and pitch.
    // Removing only its Y component fixes the eye height but shortens the
    // horizontal radius as the player looks down, which pulls the camera
    // forward. Rebuild the offset from its yaw direction at the original radius
    // so pitch remains available to engine visibility without moving the eye.
    float anchor[3] = {position[0] - offset[0],
                       position[1] - offset[1],
                       position[2] - offset[2]};
    CameraAnchor(self, anchor);
    const float followRadius = (std::max)(0.1f, radius - forwardUnits);
    float corrected[3] = {anchor[0], anchor[1] - lowerUnits, anchor[2]};
    if (horizontal > 0.0001f && followRadius > 0.0001f) {
        const float scale = followRadius / horizontal;
        corrected[0] += offset[0] * scale;
        corrected[2] += offset[2] * scale;
    }

    for (int axis = 0; axis < 3; ++axis) {
        g_applied_camera_correction[axis] = corrected[axis] - position[axis];
        position[axis] = corrected[axis];
    }
    g_corrected_camera_object = self;
}

void ObserveAfterUpdate(void* self) {
    if (!Readable(self, kScanBytes)) {
        return;
    }

    const auto* base = static_cast<const unsigned char*>(self);
    const auto* offset = reinterpret_cast<const float*>(base + 0x2d);
    const float x = offset[0];
    const float y = offset[1];
    const float z = offset[2];
    if (x != x || y != y || z != z) {
        return;
    }

    // The engine stores the camera's offset behind the player. The camera looks
    // along the opposite horizontal direction, so yaw is the heading of
    // (-x, -z). Pitch comes from the vertical component of the same vector.
    const float horizontal = std::sqrt(x * x + z * z);
    const float yaw = Degrees(std::atan2(-x, -z));
    const float pitch = horizontal > 0.0001f
                            ? Degrees(std::atan2(y, horizontal))
                            : 0.0f;

    std::lock_guard<std::mutex> lock(g_state_mutex);
    if (g_state.object != self) {
        g_state.object = self;
        ++g_state.epoch;
    }
    g_state.valid = true;
    g_state.yaw = yaw;
    g_state.pitch = pitch;
    g_state.offset[0] = x;
    g_state.offset[1] = y;
    g_state.offset[2] = z;
    if (float* position = CameraPosition(self)) {
        g_state.cameraPosition[0] = position[0];
        g_state.cameraPosition[1] = position[1];
        g_state.cameraPosition[2] = position[2];
    }
    float anchor[3] = {};
    if (CameraAnchor(self, anchor)) {
        g_state.anchor[0] = anchor[0];
        g_state.anchor[1] = anchor[1];
        g_state.anchor[2] = anchor[2];
    }
    g_state.lastUpdateMs = ::GetTickCount64();
    ++g_state.updates;
}

void NeutralizeFirstPersonSway(void* self) {
    if (!FirstPersonCameraActive() || !Readable(self, kScanBytes)) {
        return;
    }
    auto* base = static_cast<unsigned char*>(self);
    *reinterpret_cast<float*>(base + 0x65) = 0.0f;
    *reinterpret_cast<float*>(base + 0x69) = 0.0f;
    *reinterpret_cast<float*>(base + 0x6d) = 0.0f;
    *reinterpret_cast<float*>(base + 0x71) = 0.0f;
}

void __fastcall HookMouseConCamUpdate(void* self, void*) {
    NeutralizeFirstPersonSway(self);
    RestoreCameraCorrection(self);
    LookDriverApplyNativeYaw();
    if (g_real_update != nullptr) {
        g_real_update(self);
    }
    ApplyCameraCorrection(self, StereoRendererCameraLowerUnits(),
                          StereoRendererCameraForwardUnits());
    NeutralizeFirstPersonSway(self);
    ObserveAfterUpdate(self);
}

void WriteState(const CameraReadback& state) {
    const std::wstring path = JoinPath(mod::GameDirectory(), L"h47vr-camread.txt");
    FILE* file = nullptr;
    if (::_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) {
        return;
    }
    std::fprintf(file,
                 "valid=%d yaw=%.4f pitch=%.4f offset=%.4f,%.4f,%.4f "
                 "anchor=%.4f,%.4f,%.4f position=%.4f,%.4f,%.4f "
                 "camera=%p epoch=%lu updates=%lu\n",
                 state.valid ? 1 : 0, state.yaw, state.pitch, state.offset[0],
                 state.offset[1], state.offset[2], state.anchor[0],
                 state.anchor[1], state.anchor[2], state.cameraPosition[0],
                 state.cameraPosition[1], state.cameraPosition[2],
                 state.object, state.epoch, state.updates);
    std::fclose(file);
}

}  // namespace

bool CameraReadbackGet(CameraReadback* state) {
    if (state == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_state_mutex);
    *state = g_state;
    return state->valid;
}

bool CameraReadbackGameplayActive() {
    CameraReadback state;
    if (!CameraReadbackGet(&state) || state.lastUpdateMs == 0) {
        return false;
    }
    return ::GetTickCount64() - state.lastUpdateMs <= 250;
}

bool CameraReadbackInstall() {
    if (g_installed.load()) {
        return true;
    }

    HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    if (gameplay == nullptr) {
        return false;
    }

    void* target = reinterpret_cast<unsigned char*>(gameplay) +
                   kMouseConCamUpdateRva;
    if (!HookAddress(target, reinterpret_cast<void*>(&HookMouseConCamUpdate),
                     reinterpret_cast<void**>(&g_real_update))) {
        H47VR_LOG_ERROR("camera readback: could not hook MouseConCam update at %p "
                        "(rva 0x%06lX)",
                        target, kMouseConCamUpdateRva);
        return false;
    }

    g_installed = true;
    H47VR_LOG_INFO("camera readback: hooked MouseConCam update at %p "
                   "(rva 0x%06lX), state in %s",
                   target, kMouseConCamUpdateRva,
                   Narrow(JoinPath(mod::GameDirectory(), L"h47vr-camread.txt")).c_str());
    return true;
}

bool CameraReadbackRearm() {
    g_installed = false;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_state.valid = false;
        g_state.object = nullptr;
        g_state.lastUpdateMs = 0;
        ++g_state.epoch;
    }
    g_corrected_camera_object = nullptr;
    for (float& value : g_applied_camera_correction) {
        value = 0.0f;
    }
    return CameraReadbackInstall();
}

void CameraReadbackFrame(unsigned long frame) {
    CameraReadback state;
    if (!CameraReadbackGet(&state) || frame % 30 != 0) {
        return;
    }
    WriteState(state);
}

}  // namespace h47vr
