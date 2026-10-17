#include "h47vr/log.hpp"
#include "h47vr/paths.hpp"
#include "proxy_slots.h"

#include <windows.h>

namespace {

INIT_ONCE g_resolve_once = INIT_ONCE_STATIC_INIT;
HMODULE g_system_opengl32 = nullptr;

// Absolute path to the system renderer so the app-local proxy can never
// re-enter itself through the normal DLL search order.
std::wstring SystemOpenGLPath() {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = ::GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return L"opengl32.dll";
    }
    return h47vr::JoinPath(system_directory, L"opengl32.dll");
}

BOOL CALLBACK ResolveSlots(PINIT_ONCE, PVOID, PVOID*) {
    const std::wstring path = SystemOpenGLPath();
    g_system_opengl32 = ::LoadLibraryW(path.c_str());
    if (g_system_opengl32 == nullptr) {
        H47VR_LOG_ERROR("proxy: could not load system opengl32.dll from %s (error %lu)",
                        h47vr::Narrow(path).c_str(), ::GetLastError());
        return TRUE;
    }

    int resolved = 0;
    for (int i = 0; i < h47vr_proxy_count; ++i) {
        void* target = reinterpret_cast<void*>(
            ::GetProcAddress(g_system_opengl32, h47vr_proxy_names[i]));
        h47vr_proxy_slots[i] = target;
        if (target != nullptr) {
            ++resolved;
        } else {
            H47VR_LOG_WARN("proxy: system opengl32.dll has no export '%s'",
                           h47vr_proxy_names[i]);
        }
    }

    H47VR_LOG_INFO("proxy: forwarded %d of %d opengl32 exports from %s",
                   resolved, h47vr_proxy_count, h47vr::Narrow(path).c_str());
    return TRUE;
}

}  // namespace

extern "C" void h47vr_proxy_resolve(void) {
    ::InitOnceExecuteOnce(&g_resolve_once, &ResolveSlots, nullptr, nullptr);
}

// Constant-initialised, so no CRT start-up work is required before the first
// thunk can call through it.
extern "C" void (*h47vr_proxy_resolve_fn)(void) = &h47vr_proxy_resolve;
