#include "h47vr/mod.hpp"

#include <windows.h>

// Defined in gl_probe.cpp. It lives in this target because the GL seam is the
// proxy's own slot table, which no other module has.
namespace h47vr {
void GlProbePublish();
}

namespace {

// The loader lock is held while DllMain runs, so the mod does its file and
// hashing work on a short-lived worker thread instead.
DWORD WINAPI RunModStartup(LPVOID) {
    ::h47vr::mod::Initialize("opengl32-proxy");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            ::DisableThreadLibraryCalls(self);
            ::h47vr::GlProbePublish();
            HANDLE thread = ::CreateThread(nullptr, 0, &RunModStartup, nullptr, 0, nullptr);
            if (thread != nullptr) {
                ::CloseHandle(thread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            // Only when the module is freed explicitly, never while the process
            // is tearing down around us.
            if (reserved == nullptr) {
                ::h47vr::mod::Shutdown();
            }
            break;
        default:
            break;
    }
    return TRUE;
}
