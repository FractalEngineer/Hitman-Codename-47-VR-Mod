#include "h47vr/renderer_input.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace h47vr {
namespace {

constexpr unsigned long kWindowProcedureRva = 0x0002A050;
constexpr int kMouseMoveSlot = 0x194;
constexpr int kLeftDownSlot = 0x178;
constexpr int kLeftUpSlot = 0x17C;
constexpr int kWindowedClipOffset = 0x12B;

extern "C" void* h47vr_renderer_input_self = nullptr;
extern "C" void* h47vr_renderer_input_trampoline = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<unsigned long> g_native_dispatches{0};
std::atomic<unsigned long> g_button_down_dispatches{0};
std::atomic<unsigned long> g_button_up_dispatches{0};

extern "C" __declspec(naked) void h47vr_detour_renderer_input() {
    __asm {
        mov dword ptr [h47vr_renderer_input_self], ecx
        jmp dword ptr [h47vr_renderer_input_trampoline]
    }
}

void* InputObject() {
    return h47vr_renderer_input_self;
}

void* InputSlot(void* self, int offset) {
    if (self == nullptr) {
        return nullptr;
    }
    void** vtable = *reinterpret_cast<void***>(self);
    if (vtable == nullptr) {
        return nullptr;
    }
    return vtable[offset / 4];
}

bool Dispatch(const char* name, int slot, unsigned int flags, int x, int y) {
    void* self = InputObject();
    void* target = InputSlot(self, slot);
    if (self == nullptr || target == nullptr) {
        return false;
    }

    auto* bytes = static_cast<unsigned char*>(self);
    const unsigned char previousClip = bytes[kWindowedClipOffset];
    bytes[kWindowedClipOffset] = 1;
    using MouseHandlerFn =
        void(__thiscall*)(void*, unsigned int flags, int x, int y);
    reinterpret_cast<MouseHandlerFn>(target)(self, flags, x, y);
    bytes[kWindowedClipOffset] = previousClip;

    if (g_native_dispatches.fetch_add(1) == 0) {
        H47VR_LOG_INFO("renderer input: dispatching menu mouse events through %s",
                       name);
    }
    return true;
}

}  // namespace

bool RendererInputStart() {
    RendererInputRearm();
    return true;
}

void RendererInputRearm() {
    if (g_installed.load()) {
        return;
    }
    HMODULE renderer = LoadedModule(L"RenderOpenGL.dll");
    if (renderer == nullptr) {
        return;
    }
    void* target =
        reinterpret_cast<unsigned char*>(renderer) + kWindowProcedureRva;
    if (!HookAddress(target,
                     reinterpret_cast<void*>(&h47vr_detour_renderer_input),
                     reinterpret_cast<void**>(
                         &h47vr_renderer_input_trampoline))) {
        H47VR_LOG_ERROR("renderer input: could not hook window procedure at %p "
                        "(rva 0x%06lX)",
                        target, kWindowProcedureRva);
        return;
    }
    h47vr_renderer_input_self = nullptr;
    g_installed = true;
    H47VR_LOG_INFO("renderer input: native mouse dispatch hooked at %p "
                   "(rva 0x%06lX)",
                   target, kWindowProcedureRva);
}

bool RendererInputAvailable() {
    return InputObject() != nullptr;
}

bool RendererInputMove(int x, int y) {
    return Dispatch("move", kMouseMoveSlot, 0, x, y);
}

bool RendererInputButton(bool down, int x, int y) {
    const bool dispatched = Dispatch(down ? "left-down" : "left-up",
                                     down ? kLeftDownSlot : kLeftUpSlot,
                                     down ? MK_LBUTTON : 0, x, y);
    if (dispatched && down && g_button_down_dispatches++ == 0) {
        H47VR_LOG_INFO("renderer input: native menu left-down dispatched at %d,%d",
                       x, y);
    }
    if (dispatched && !down && g_button_up_dispatches++ == 0) {
        H47VR_LOG_INFO("renderer input: native menu left-up dispatched at %d,%d",
                       x, y);
    }
    return dispatched;
}

}  // namespace h47vr
