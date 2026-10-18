#include "h47vr/window_keepalive.hpp"

#include "h47vr/log.hpp"

#include <windows.h>

#include <atomic>

namespace h47vr {
namespace {

std::atomic<HWND> g_window{nullptr};
std::atomic<unsigned long> g_swallowed{0};
std::atomic<unsigned long> g_passed{0};
WNDPROC g_original = nullptr;

LRESULT CALLBACK KeepaliveProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
        // The activation messages are the ones that stop the engine drawing.
        // Returning zero without passing them on is the whole trick: the window
        // procedure is never told it was deactivated, so the engine keeps its
        // frame loop running.
        case WM_ACTIVATE:
            if (LOWORD(wparam) == WA_INACTIVE) {
                ++g_swallowed;
                return 0;
            }
            break;
        case WM_ACTIVATEAPP:
            if (wparam == FALSE) {
                ++g_swallowed;
                return 0;
            }
            break;
        case WM_NCACTIVATE:
            if (wparam == FALSE) {
                ++g_swallowed;
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            ++g_swallowed;
            return 0;
        default:
            break;
    }

    ++g_passed;
    return ::CallWindowProcW(g_original, window, message, wparam, lparam);
}

struct PickWindow {
    DWORD processId;
    HWND best;
    long bestArea;
};

BOOL CALLBACK PickWindowProc(HWND window, LPARAM param) {
    auto* pick = reinterpret_cast<PickWindow*>(param);

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid != pick->processId) {
        return TRUE;
    }

    RECT bounds{};
    if (!::GetWindowRect(window, &bounds)) {
        return TRUE;
    }
    const long area = (bounds.right - bounds.left) * (bounds.bottom - bounds.top);
    if (area > pick->bestArea) {
        pick->bestArea = area;
        pick->best = window;
    }
    return TRUE;
}

// The window is created by the engine, usually within a second of the process
// starting, but the mod has no hook on its creation. Polling is simple and the
// cost is a handful of enumeration calls.
DWORD WINAPI KeepaliveWorker(LPVOID) {
    const DWORD processId = ::GetCurrentProcessId();

    for (int attempt = 0; attempt < 900; ++attempt) {
        if (g_window.load() != nullptr) {
            return 0;
        }

        PickWindow pick{processId, nullptr, 0};
        ::EnumWindows(&PickWindowProc, reinterpret_cast<LPARAM>(&pick));
        if (pick.best != nullptr) {
            const LONG_PTR previous = ::SetWindowLongPtrW(
                pick.best, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&KeepaliveProc));
            if (previous != 0) {
                g_original = reinterpret_cast<WNDPROC>(previous);
                g_window = pick.best;
                RECT bounds{};
                ::GetWindowRect(pick.best, &bounds);
                H47VR_LOG_INFO(
                    "window keepalive: subclassed hwnd %p (%ldx%ld) - the engine will "
                    "keep rendering while the window is not active",
                    pick.best, bounds.right - bounds.left, bounds.bottom - bounds.top);
                return 0;
            }
            H47VR_LOG_WARN("window keepalive: could not subclass hwnd %p (error %lu)",
                           pick.best, ::GetLastError());
        }

        ::Sleep(100);
    }

    H47VR_LOG_WARN("window keepalive: no window found to subclass");
    return 0;
}

}  // namespace

bool WindowKeepaliveStart() {
    HANDLE thread = ::CreateThread(nullptr, 0, &KeepaliveWorker, nullptr, 0, nullptr);
    if (thread == nullptr) {
        H47VR_LOG_ERROR("window keepalive: could not start the worker (error %lu)",
                        ::GetLastError());
        return false;
    }
    ::CloseHandle(thread);
    return true;
}

unsigned long WindowKeepaliveSwallowed() {
    return g_swallowed.load();
}

}  // namespace h47vr
