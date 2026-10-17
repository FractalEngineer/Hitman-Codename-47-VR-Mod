#include "h47vr/mod.hpp"

#include <windows.h>

namespace {

DWORD WINAPI RunModStartup(LPVOID) {
    ::h47vr::mod::Initialize("h47vr_mod.dll");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH: {
            ::DisableThreadLibraryCalls(self);
            HANDLE thread = ::CreateThread(nullptr, 0, &RunModStartup, nullptr, 0, nullptr);
            if (thread != nullptr) {
                ::CloseHandle(thread);
            }
            break;
        }
        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) {
                ::h47vr::mod::Shutdown();
            }
            break;
        default:
            break;
    }
    return TRUE;
}
