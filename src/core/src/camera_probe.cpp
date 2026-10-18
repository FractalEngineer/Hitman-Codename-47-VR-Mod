#include "h47vr/camera_probe.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>

namespace h47vr {

// Defined below, outside the anonymous namespace, so the detour observer can
// reach it by a plain name.
void DumpCamera(void* camera, const char* why);

namespace {

// `FUN_0fd53c50`: eye = R * (world - camPos), with the camera as the last stack
// argument.
constexpr unsigned long kCameraTransformRva = 0x00093C50;

// Next lead, not yet hooked: `FUN_0fd56120` is ZCAMERA.cpp's CamUpdate. It is
// the engine frame on the stack when the world projection is set up (found by
// sampling at `glFrustum`, HitmanDlc.dlc +0x964A3), it logs the string
// "CamUpdate", and it calls the camera's own virtuals - so its first argument is
// the camera object this project has been looking for.
constexpr unsigned long kCameraUpdateRva = 0x00096120;

extern "C" void* h47vr_camera_trampoline = nullptr;
extern "C" void* h47vr_camera_args_target = nullptr;

std::atomic<void*> g_camera{nullptr};
std::atomic<unsigned long> g_calls{0};
std::wstring g_gate;

// Distinct camera pointers seen so far, so a per-object argument cannot be
// mistaken for the camera: if this is really the camera it is the same pointer
// every time.
constexpr int kMaxDistinct = 8;
void* g_distinct[kMaxDistinct] = {};
int g_distinct_count = 0;
bool g_dumped = false;

// Entry layout: [esp] = return address, ECX = this, stack arguments from
// [esp+4]. The thunk preserves every register and tail-jumps to the original
// bytes, so it assumes nothing about the convention or the argument count - it
// only reads the arguments, and the original `ret` still does the cleanup.
extern "C" __declspec(naked) void h47vr_detour_camera_transform() {
    __asm {
        pushfd
        pushad
        // After pushfd+pushad: eflags is at [esp+32], the return address at
        // [esp+36] and the stack arguments from [esp+40] upward. Pushing the
        // same slot four times walks the arguments out in reverse order,
        // because each push moves the next one up by four.
        push dword ptr [esp+52]
        push dword ptr [esp+52]
        push dword ptr [esp+52]
        push dword ptr [esp+52]
        call dword ptr [h47vr_camera_args_target]
        add esp, 16
        popad
        popfd
        jmp dword ptr [h47vr_camera_trampoline]
    }
}

// Committed, readable pages only. The camera pointer comes out of live engine
// state, so it is worth one cheap check before dereferencing it.
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

void ObserveCameraTransform(void* a1, void* a2, void* a3, void* a4) {
    const unsigned long call = ++g_calls;

    // The camera is the last argument. It is only accepted when it actually
    // points at something readable, so a convention surprise cannot leave a
    // wild pointer behind for the report to dereference.
    if (a4 != nullptr && Readable(a4, 0x20)) {
        g_camera = a4;
        bool known = false;
        for (int i = 0; i < g_distinct_count; ++i) {
            if (g_distinct[i] == a4) {
                known = true;
                break;
            }
        }
        if (!known && g_distinct_count < kMaxDistinct) {
            g_distinct[g_distinct_count++] = a4;
            H47VR_LOG_INFO("camera transform: new last-argument pointer %p (call %lu)",
                           a4, call);
        }
    }

    if (call <= 6) {
        H47VR_LOG_INFO("camera transform #%lu args: %p %p %p %p",
                       call, a1, a2, a3, a4);
    }

    // Dump as soon as a camera exists, rather than waiting for a frame report:
    // an opening sequence can wedge the engine with no frames at all, and the
    // whole point is to know the layout before that happens.
    if (!g_dumped && g_camera.load() != nullptr) {
        g_dumped = true;
        DumpCamera(g_camera.load(), "first sighting");
    }
}

}  // namespace

void* CameraProbeObject() {
    return g_camera.load();
}

bool CameraProbeInstall() {
    HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    if (gameplay == nullptr) {
        H47VR_LOG_WARN("camera probe: HitmanDlc.dlc is not loaded");
        return false;
    }

    g_gate = JoinPath(mod::GameDirectory(), L"h47vr-camobj.txt");
    h47vr_camera_args_target = reinterpret_cast<void*>(&ObserveCameraTransform);

    void* target = reinterpret_cast<char*>(gameplay) + kCameraTransformRva;
    if (!HookAddress(target, reinterpret_cast<void*>(&h47vr_detour_camera_transform),
                     &h47vr_camera_trampoline)) {
        H47VR_LOG_ERROR("camera probe: could not hook HitmanDlc.dlc rva 0x%06lX at %p",
                        kCameraTransformRva, target);
        return false;
    }
    H47VR_LOG_INFO("hooked camera transform HitmanDlc.dlc rva 0x%06lX at %p",
                   kCameraTransformRva, target);
    return true;
}

bool CameraProbeRearm() {
    return CameraProbeInstall();
}

void DumpCamera(void* camera, const char* why) {
    if (camera == nullptr) {
        H47VR_LOG_INFO("camera object: none seen yet (%lu transform calls)",
                       g_calls.load());
        return;
    }

    H47VR_LOG_INFO("camera object %p (%s), %lu transform calls since install",
                   camera, why, g_calls.load());

    // The layout the decompilation implies: a vtable, a pointer to the 3x3 view
    // rotation, then the camera position as three floats at +8, +0xc, +0x10.
    const auto* words = reinterpret_cast<const unsigned int*>(camera);
    H47VR_LOG_INFO("  raw   %08X %08X %08X %08X %08X %08X %08X %08X",
                   words[0], words[1], words[2], words[3],
                   words[4], words[5], words[6], words[7]);

    const auto* floats = reinterpret_cast<const float*>(camera);
    H47VR_LOG_INFO("  float %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f",
                   floats[0], floats[1], floats[2], floats[3],
                   floats[4], floats[5], floats[6], floats[7]);

    const void* rotation = reinterpret_cast<const void*>(
        static_cast<std::uintptr_t>(words[1]));
    if (Readable(rotation, 9 * sizeof(float))) {
        const auto* m = reinterpret_cast<const float*>(rotation);
        H47VR_LOG_INFO("  view rotation at %p | %8.4f %8.4f %8.4f | %8.4f %8.4f %8.4f "
                       "| %8.4f %8.4f %8.4f",
                       rotation, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
    } else {
        H47VR_LOG_INFO("  field +4 (%p) is not a readable 3x3", rotation);
    }
}

void CameraProbeReport(unsigned long frame) {
    if (::GetFileAttributesW(g_gate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    H47VR_LOG_INFO("frame %lu camera probe report", frame);
    DumpCamera(g_camera.load(), "periodic");
}

}  // namespace h47vr
