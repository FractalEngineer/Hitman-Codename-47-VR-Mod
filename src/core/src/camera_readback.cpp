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
constexpr int kScanBytes = 0x93;

using MouseConCamUpdateFn = void(__fastcall*)(void* self);
MouseConCamUpdateFn g_real_update = nullptr;

std::atomic<bool> g_installed{false};
std::mutex g_state_mutex;
CameraReadback g_state;
void* g_lowered_camera_object = nullptr;
float g_applied_camera_lower = 0.0f;

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

void RestoreCameraLower(void* self) {
    if (self == nullptr || self != g_lowered_camera_object ||
        g_applied_camera_lower == 0.0f) {
        return;
    }
    float* position = CameraPosition(self);
    if (position != nullptr) {
        position[1] += g_applied_camera_lower;
    }
    g_applied_camera_lower = 0.0f;
}

void ApplyCameraLower(void* self, float lowerUnits) {
    float* position = CameraPosition(self);
    if (position == nullptr || !std::isfinite(lowerUnits) ||
        !Readable(self, 0x39)) {
        g_lowered_camera_object = nullptr;
        g_applied_camera_lower = 0.0f;
        return;
    }
    const auto* offset = reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(self) + 0x2d);
    const float correction = offset[1] + lowerUnits;
    position[1] -= correction;
    g_lowered_camera_object = self;
    g_applied_camera_lower = correction;
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
    RestoreCameraLower(self);
    LookDriverApplyNativeYaw();
    if (g_real_update != nullptr) {
        g_real_update(self);
    }
    ApplyCameraLower(self, StereoRendererCameraLowerUnits());
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
                 "position=%.4f,%.4f,%.4f camera=%p epoch=%lu updates=%lu\n",
                 state.valid ? 1 : 0, state.yaw, state.pitch, state.offset[0],
                 state.offset[1], state.offset[2], state.cameraPosition[0],
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
    g_lowered_camera_object = nullptr;
    g_applied_camera_lower = 0.0f;
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
