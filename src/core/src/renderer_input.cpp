#include "h47vr/renderer_input.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/look_driver.hpp"
#include "h47vr/symbols.hpp"
#include "h47vr/window_keepalive.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace h47vr {
namespace {

constexpr unsigned long kWindowProcedureRva = 0x0002A050;
constexpr int kMouseMoveSlot = 0x194;
constexpr int kLeftDownSlot = 0x178;
constexpr int kLeftUpSlot = 0x17C;
constexpr int kKeyDownSlot = 0x170;
constexpr int kKeyUpSlot = 0x174;
constexpr int kWindowedClipOffset = 0x12B;
constexpr int kMouseCaptureOffset = 0x5B;
constexpr int kFullscreenInterfaceOffset = 0x38F1;

extern "C" void* h47vr_renderer_input_self = nullptr;
extern "C" void* h47vr_renderer_input_trampoline = nullptr;

std::atomic<bool> g_installed{false};
std::atomic<unsigned long> g_native_dispatches{0};
std::atomic<unsigned long> g_button_down_dispatches{0};
std::atomic<unsigned long> g_button_up_dispatches{0};
std::atomic<unsigned long> g_key_dispatches{0};

extern "C" __declspec(naked) void h47vr_detour_renderer_input() {
    __asm {
        mov dword ptr [h47vr_renderer_input_self], ecx
        jmp dword ptr [h47vr_renderer_input_trampoline]
    }
}

void* InputObject() {
    return h47vr_renderer_input_self;
}

bool SendWindowInput(UINT message, WPARAM flags, int x, int y) {
    void* self = InputObject();
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    if (self == nullptr || window == nullptr) {
        return false;
    }
    auto* bytes = static_cast<unsigned char*>(self);
    const unsigned char previousMode = bytes[0x81];
    const unsigned char previousClip = bytes[kWindowedClipOffset];
    const unsigned char previousCaptured = bytes[kMouseCaptureOffset];
    bytes[0x81] = 0;
    bytes[kWindowedClipOffset] = 1;
    bytes[kMouseCaptureOffset] = 0;
    const LPARAM position =
        MAKELPARAM(static_cast<WORD>(x), static_cast<WORD>(y));
    LookDriverBeginSyntheticMouse();
    ::SendMessageW(window, message, flags, position);
    LookDriverEndSyntheticMouse();
    bytes[0x81] = previousMode;
    bytes[kWindowedClipOffset] = previousClip;
    bytes[kMouseCaptureOffset] = previousCaptured;
    return true;
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

    void* interfaceSlot = ResolveSymbol(
        L"Globals.dll", "?g_pSysInterface@@3PAVZSysInterface@@A");
    auto* sysInterface = interfaceSlot != nullptr
                             ? *reinterpret_cast<unsigned char**>(interfaceSlot)
                             : nullptr;
    auto* bytes = static_cast<unsigned char*>(self);
    const unsigned char previousClip = bytes[kWindowedClipOffset];
    const unsigned char previousCaptured = bytes[kMouseCaptureOffset];
    const unsigned char previousFullscreen =
        sysInterface != nullptr ? sysInterface[kFullscreenInterfaceOffset] : 0;

    // The windowed relative-mouse path treats these coordinates as deltas from
    // the client origin. The renderer already has an absolute branch for the
    // fullscreen input mode, and it is exactly the UI pointer this controller
    // needs. Select that branch only for the synchronous native dispatch.
    bytes[kWindowedClipOffset] = 1;
    bytes[kMouseCaptureOffset] = 0;
    if (sysInterface != nullptr) {
        sysInterface[kFullscreenInterfaceOffset] = 1;
    }
    using MouseHandlerFn =
        void(__thiscall*)(void*, unsigned int flags, int x, int y);
    reinterpret_cast<MouseHandlerFn>(target)(self, flags, x, y);
    bytes[kWindowedClipOffset] = previousClip;
    bytes[kMouseCaptureOffset] = previousCaptured;
    if (sysInterface != nullptr) {
        sysInterface[kFullscreenInterfaceOffset] = previousFullscreen;
    }

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
    if (SendWindowInput(WM_MOUSEMOVE, 0, x, y)) {
        return true;
    }
    return Dispatch("move", kMouseMoveSlot, 0, x, y);
}

bool RendererInputButton(bool down, int x, int y) {
    if (SendWindowInput(down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                        down ? MK_LBUTTON : 0, x, y)) {
        return true;
    }
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

bool RendererInputKey(int vk, bool down) {
    void* self = InputObject();
    void* target = InputSlot(self, down ? kKeyDownSlot : kKeyUpSlot);
    if (self == nullptr || target == nullptr || vk <= 0) {
        return false;
    }

    const unsigned int scan =
        ::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC);
    unsigned int lparam = 1u | ((scan & 0xFFu) << 16);
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
        lparam |= 0x01000000u;
    }
    if (!down) {
        lparam |= 0xC0000000u;
    }

    using KeyHandlerFn =
        void(__thiscall*)(void*, unsigned int vk, unsigned int flags);
    reinterpret_cast<KeyHandlerFn>(target)(
        self, static_cast<unsigned int>(vk), lparam);
    if (g_key_dispatches.fetch_add(1) == 0) {
        H47VR_LOG_INFO("renderer input: dispatching keyboard state through the "
                       "native window input object");
    }
    return true;
}

}  // namespace h47vr
