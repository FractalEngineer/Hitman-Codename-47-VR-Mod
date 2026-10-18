#include "h47vr/renderer_probe.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <cstdint>

// The counters and trampolines have C linkage so the naked thunks can address
// them by a plain symbol, and are non-const so they get external linkage.
extern "C" {
unsigned long h47vr_render_counts[4] = {0, 0, 0, 0};
void* h47vr_render_trampolines[4] = {nullptr, nullptr, nullptr, nullptr};
}

// Each thunk preserves flags, bumps its counter, then continues into the
// original code. Arguments and registers are untouched.
extern "C" __declspec(naked) void h47vr_detour_present() {
    __asm {
        pushfd
        inc dword ptr [h47vr_render_counts + 0]
        popfd
        jmp dword ptr [h47vr_render_trampolines + 0]
    }
}

extern "C" __declspec(naked) void h47vr_detour_frame_draw() {
    __asm {
        pushfd
        inc dword ptr [h47vr_render_counts + 4]
        popfd
        jmp dword ptr [h47vr_render_trampolines + 4]
    }
}

extern "C" __declspec(naked) void h47vr_detour_object_draw_a() {
    __asm {
        pushfd
        inc dword ptr [h47vr_render_counts + 8]
        popfd
        jmp dword ptr [h47vr_render_trampolines + 8]
    }
}

extern "C" __declspec(naked) void h47vr_detour_object_draw_b() {
    __asm {
        pushfd
        inc dword ptr [h47vr_render_counts + 12]
        popfd
        jmp dword ptr [h47vr_render_trampolines + 12]
    }
}

namespace h47vr {
namespace {

struct RendererTarget {
    const char* label;
    unsigned long rva;
};

// RVAs, not addresses: RenderOpenGL.dll relocates on this machine.
constexpr RendererTarget kTargets[] = {
    {"present (the only caller of SwapBuffers)", 0x000048F0},
    {"frame draw (RenderWintelOpenGLDraw.cpp assertions)", 0x00003680},
    {"object draw (glDrawElements, model matrix)", 0x00006590},
    {"object draw variant (CullFace, matrix stack)", 0x00006270},
};

bool g_installed = false;

}  // namespace

bool RendererProbeInstall() {
    if (g_installed) {
        return true;
    }

    HMODULE renderer = LoadedModule(L"RenderOpenGL.dll");
    if (renderer == nullptr) {
        H47VR_LOG_WARN("renderer probe: RenderOpenGL.dll is not loaded");
        return false;
    }

    void* detours[] = {
        reinterpret_cast<void*>(&h47vr_detour_present),
        reinterpret_cast<void*>(&h47vr_detour_frame_draw),
        reinterpret_cast<void*>(&h47vr_detour_object_draw_a),
        reinterpret_cast<void*>(&h47vr_detour_object_draw_b),
    };

    const auto base = reinterpret_cast<std::uintptr_t>(renderer);
    int installed = 0;
    for (int i = 0; i < 4; ++i) {
        void* target = reinterpret_cast<void*>(base + kTargets[i].rva);
        // Hook straight into the stored slot: on a re-install after a module
        // reload MinHook reports the hook as already created and leaves the
        // trampoline untouched, so it must not be cleared first.
        if (HookAddress(target, detours[i], &h47vr_render_trampolines[i])) {
            ++installed;
            H47VR_LOG_INFO("hooked renderer %-52s rva 0x%06lX at %p",
                           kTargets[i].label, kTargets[i].rva, target);
        } else {
            H47VR_LOG_WARN("could not hook renderer rva 0x%06lX (%s)",
                           kTargets[i].rva, kTargets[i].label);
        }
    }

    g_installed = installed > 0;
    return g_installed;
}

void RendererProbeReport(unsigned long frame) {
    if (!g_installed) {
        return;
    }
    H47VR_LOG_INFO("frame %lu renderer calls: present=%lu frameDraw=%lu objectDrawA=%lu objectDrawB=%lu",
                   frame,
                   h47vr_render_counts[0], h47vr_render_counts[1],
                   h47vr_render_counts[2], h47vr_render_counts[3]);
}

void RendererProbeReset() {
    g_installed = false;
}

void RendererProbeReportState(const char* when) {
    HMODULE renderer = LoadedModule(L"RenderOpenGL.dll");
    if (renderer == nullptr) {
        H47VR_LOG_WARN("renderer state (%s): RenderOpenGL.dll is not loaded", when);
        return;
    }

    // MinHook patches the entry with a relative jump. Reading that byte back is
    // the only way to tell whether the module was unmapped and remapped, which
    // silently discards the patch.
    const auto* entry = reinterpret_cast<const unsigned char*>(renderer) + kTargets[0].rva;
    const unsigned char first = *entry;
    const bool patched = first == 0xE9;
    H47VR_LOG_INFO("renderer state (%s): base %p, rva %06lX first byte 0x%02X (%s)",
                   when, renderer, kTargets[0].rva, first,
                   patched ? "patched" : "original - the hook is gone");

    if (!patched) {
        H47VR_LOG_WARN("renderer detour was lost (module reloaded); reinstalling");
        RendererProbeReset();
        RendererProbeInstall();
    }
}

}  // namespace h47vr
