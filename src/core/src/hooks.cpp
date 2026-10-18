#include "h47vr/hooks.hpp"

#include "h47vr/log.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"

#include <MinHook.h>

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace h47vr {
namespace {

std::mutex g_mutex;
bool g_initialized = false;
PVOID g_watch_cookie = nullptr;

struct WatchEntry {
    std::wstring module;
    void (*callback)();
    std::atomic<bool> fired{false};
};

std::vector<WatchEntry*> g_watch_entries;

// --- loader notification ----------------------------------------------------
//
// UNICODE_STRING lives in winternl.h, which conflicts with windows.h, so the
// layout is declared locally. It is a stable, documented structure.
struct UnicodeStringLike {
    USHORT length;
    USHORT maximumLength;
    const wchar_t* buffer;
};

// The loaded and unloaded notification structs share a layout, so one
// definition covers both.
struct LdrDllNotificationData {
    ULONG flags;
    const UnicodeStringLike* fullDllName;
    const UnicodeStringLike* baseDllName;
    PVOID dllBase;
    ULONG sizeOfImage;
};

using LdrNotificationCallback = VOID(CALLBACK*)(ULONG, const LdrDllNotificationData*, PVOID);
using LdrRegisterNotification = NTSTATUS(NTAPI*)(ULONG, LdrNotificationCallback, PVOID, PVOID*);
using LdrUnregisterNotification = NTSTATUS(NTAPI*)(PVOID);

constexpr ULONG kDllNotificationLoaded = 1;

struct PendingCallback {
    void (*callback)();
};

DWORD WINAPI RunDeferredCallback(LPVOID parameter) {
    PendingCallback* pending = static_cast<PendingCallback*>(parameter);
    void (*callback)() = pending->callback;
    delete pending;

    // Deliberately off the loader lock: patching code while the loader is
    // serialising module loads is asking for trouble.
    callback();
    return 0;
}

std::wstring BaseNameOf(const UnicodeStringLike* name) {
    if (name == nullptr || name->buffer == nullptr) {
        return {};
    }
    return std::wstring(name->buffer, name->length / sizeof(wchar_t));
}

VOID CALLBACK OnDllNotification(ULONG reason, const LdrDllNotificationData* data, PVOID) {
    if (reason != kDllNotificationLoaded || data == nullptr) {
        return;
    }
    const std::wstring baseName = BaseNameOf(data->baseDllName);

    std::lock_guard<std::mutex> lock(g_mutex);
    for (WatchEntry* entry : g_watch_entries) {
        if (_wcsicmp(entry->module.c_str(), baseName.c_str()) != 0) {
            continue;
        }
        bool expected = false;
        if (!entry->fired.compare_exchange_strong(expected, true)) {
            continue;
        }
        H47VR_LOG_INFO("module loaded: %s - running deferred hooks",
                       Narrow(baseName).c_str());
        PendingCallback* pending = new PendingCallback{entry->callback};
        HANDLE thread = ::CreateThread(nullptr, 0, &RunDeferredCallback, pending, 0, nullptr);
        if (thread != nullptr) {
            ::CloseHandle(thread);
        } else {
            delete pending;
        }
    }
}

}  // namespace

bool HooksInitialize() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_initialized) {
        return true;
    }
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        H47VR_LOG_ERROR("MH_Initialize failed (%d)", static_cast<int>(status));
        return false;
    }
    g_initialized = true;
    H47VR_LOG_INFO("hook engine ready (MinHook)");
    return true;
}

void HooksShutdown() {
    StopModuleWatch();

    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_initialized) {
        return;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    g_initialized = false;
}

bool HookAddress(void* target, void* detour, void** original) {
    if (target == nullptr || detour == nullptr) {
        return false;
    }
    if (!HooksInitialize()) {
        return false;
    }

    const MH_STATUS created = MH_CreateHook(target, detour, original);
    if (created != MH_OK) {
        H47VR_LOG_ERROR("MH_CreateHook(%p) failed (%d)", target, static_cast<int>(created));
        return false;
    }
    const MH_STATUS enabled = MH_EnableHook(target);
    if (enabled != MH_OK) {
        H47VR_LOG_ERROR("MH_EnableHook(%p) failed (%d)", target, static_cast<int>(enabled));
        return false;
    }
    return true;
}

bool HookSymbol(const wchar_t* moduleName, const char* symbol, void* detour, void** original) {
    void* target = ResolveSymbol(moduleName, symbol);
    if (target == nullptr) {
        H47VR_LOG_WARN("cannot hook %s!%s: not resolved",
                       Narrow(moduleName).c_str(), symbol);
        return false;
    }
    if (!HookAddress(target, detour, original)) {
        return false;
    }
    H47VR_LOG_INFO("hooked %s!%s at %p", Narrow(moduleName).c_str(), symbol, target);
    return true;
}

bool WatchModuleLoad(const wchar_t* moduleName, void (*callback)()) {
    if (moduleName == nullptr || callback == nullptr) {
        return false;
    }

    HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (ntdll == nullptr) {
        return false;
    }
    auto register_ = reinterpret_cast<LdrRegisterNotification>(
        ::GetProcAddress(ntdll, "LdrRegisterDllNotification"));
    if (register_ == nullptr) {
        H47VR_LOG_WARN("LdrRegisterDllNotification is unavailable");
        return false;
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_watch_cookie == nullptr) {
        PVOID cookie = nullptr;
        if (register_(0, &OnDllNotification, nullptr, &cookie) != 0) {
            H47VR_LOG_ERROR("LdrRegisterDllNotification failed");
            return false;
        }
        g_watch_cookie = cookie;
    }

    WatchEntry* entry = new WatchEntry{moduleName, callback};
    g_watch_entries.push_back(entry);
    H47VR_LOG_INFO("watching for module load: %s", Narrow(moduleName).c_str());
    return true;
}

void StopModuleWatch() {
    std::vector<WatchEntry*> entries;
    PVOID cookie = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        entries.swap(g_watch_entries);
        cookie = g_watch_cookie;
        g_watch_cookie = nullptr;
    }

    if (cookie != nullptr) {
        HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        auto unregister_ = reinterpret_cast<LdrUnregisterNotification>(
            ::GetProcAddress(ntdll, "LdrUnregisterDllNotification"));
        if (unregister_ != nullptr) {
            unregister_(cookie);
        }
    }

    for (WatchEntry* entry : entries) {
        delete entry;
    }
}

}  // namespace h47vr
