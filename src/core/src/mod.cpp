#include "h47vr/mod.hpp"

#include "h47vr/console_driver.hpp"
#include "h47vr/camera_probe.hpp"
#include "h47vr/camera_readback.hpp"
#include "h47vr/fingerprint.hpp"
#include "h47vr/gl_probe.hpp"
#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/look_driver.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/renderer_probe.hpp"
#include "h47vr/scene_probe.hpp"
#include "h47vr/symbols.hpp"
#include "h47vr/version.hpp"
#include "h47vr/window_keepalive.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <atomic>

namespace h47vr::mod {
namespace {

std::mutex g_mutex;
bool g_started = false;
bool g_compatible = false;
std::wstring g_gameDirectory;

// First non-empty line of a small text file, trimmed. Used for the one-shot
// control files the tools drop next to the game; a missing file yields an empty
// string.
std::string FirstLine(const std::wstring& path) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                 line.back() == ' ' || line.back() == '\t')) {
            line.pop_back();
        }
        const auto first = line.find_first_not_of(" \t");
        if (first == std::string::npos) {
            continue;
        }
        line.erase(0, first);
        if (line[0] == '#') {
            continue;
        }
        return line;
    }
    return {};
}

void LogSystemContext() {
    SYSTEM_INFO info{};
    // GetSystemInfo, not GetNativeSystemInfo: the latter reports the host
    // architecture, which is x64 for a 32-bit process running under WOW64.
    ::GetSystemInfo(&info);
    BOOL wow64 = FALSE;
    ::IsWow64Process(::GetCurrentProcess(), &wow64);
    H47VR_LOG_INFO("process architecture: %s%s",
                   info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL
                       ? "x86" : "not x86",
                   wow64 ? " (WOW64)" : "");

    const std::wstring exe = ModulePath(nullptr);
    H47VR_LOG_INFO("host image: %s", Narrow(exe).c_str());

    wchar_t windows[MAX_PATH]{};
    const UINT length = ::GetSystemDirectoryW(windows, MAX_PATH);
    H47VR_LOG_INFO("system directory: %s",
                   Narrow(std::wstring(windows, length)).c_str());
}

struct SymbolProbe {
    const wchar_t* module;
    const char* symbol;
    const char* purpose;
};

// The hook map depends on these. Resolving by name rather than by address means
// a build whose export table changed cannot be silently mis-hooked, and the
// build fingerprint already refuses to patch such a build.
constexpr SymbolProbe kProbes[] = {
    {L"Globals.dll", "?g_pSysInput@@3PAVZSysInput@@A", "input singleton"},
    {L"System.dll", "RunEngine", "engine entry"},
    {L"HitmanDlc.dlc", "_ZCAMERA_BaseCamera", "camera base"},
    {L"HitmanDlc.dlc", "_ZCAMERA_MouseConCam", "gameplay camera"},
    {L"HitmanDlc.dlc", "_ZCAMERA_MainCameraControl", "camera control"},
    {L"HitmanDlc.dlc", "_ZCAMERA_NewFollowCam2", "follow camera"},
    {L"HitmanDlc.dlc", "_ZLIST_SortCameraControls", "camera selection"},
    {L"HitmanDlc.dlc", "_ZGEOM_AdoptVisibility", "visibility"},
    {L"HitmanDlc.dlc", "_ZLIST_CheckVisible", "visibility query"},
    {L"HitmanDlc.dlc", "_ZITEM_FireShot", "weapon fire"},
    {L"HitmanDlc.dlc", "_ZGEOM_BasePickupTarget", "interaction target"},
    {L"HitmanDlc.dlc", "_ZGEOM_ItemActivate", "interaction activate"},
};

void LogSymbolProbes() {
    H47VR_LOG_INFO("resolving hook-map symbols by name:");
    for (const SymbolProbe& probe : kProbes) {
        const std::string moduleName = Narrow(probe.module);

        HMODULE module = LoadedModule(probe.module);
        if (module == nullptr) {
            H47VR_LOG_WARN("  %-15s %-28s not loaded yet  (%s)",
                           moduleName.c_str(), probe.symbol, probe.purpose);
            continue;
        }

        void* address = ResolveSymbol(probe.module, probe.symbol);
        if (address == nullptr) {
            H47VR_LOG_ERROR("  %-15s %-28s MISSING         (%s)",
                            moduleName.c_str(), probe.symbol, probe.purpose);
            continue;
        }

        const auto offset = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(address) -
            reinterpret_cast<std::uintptr_t>(module));
        H47VR_LOG_INFO("  %-15s %-28s %p  rva 0x%06llX  (%s)",
                       moduleName.c_str(), probe.symbol, address, offset, probe.purpose);
    }
}

// --- hooks -----------------------------------------------------------------

// The engine routes its console and debug output through OutputDebugString
// (SysComWintel::DataToDebug). Hooking it turns the game's own console into
// something the mod can read, which is how console commands get discovered and
// verified without a debugger attached.
thread_local bool g_in_debug_output = false;

using OutputDebugStringAFn = void(WINAPI*)(LPCSTR);
OutputDebugStringAFn g_real_output_debug_string = nullptr;
std::atomic<unsigned long> g_debug_lines{0};

using OutputDebugStringWFn = void(WINAPI*)(LPCWSTR);
OutputDebugStringWFn g_real_output_debug_string_w = nullptr;

void WINAPI HookOutputDebugStringA(LPCSTR text) {
    // The logger calls this API, so anything arriving while the logger is
    // active is our own output and must not be fed back into the logger.
    if (text != nullptr && !g_in_debug_output && !LoggerActive()) {
        g_in_debug_output = true;
        const char* cursor = text;
        while (*cursor == '\r' || *cursor == '\n') {
            ++cursor;
        }
        if (*cursor != '\0') {
            ++g_debug_lines;
            H47VR_LOG_INFO("engine: %s", cursor);
        }
        g_in_debug_output = false;
    }
    g_real_output_debug_string(text);
}

void WINAPI HookOutputDebugStringW(LPCWSTR text) {
    if (text != nullptr && !g_in_debug_output && !LoggerActive()) {
        g_in_debug_output = true;
        const wchar_t* cursor = text;
        while (*cursor == L'\r' || *cursor == L'\n') {
            ++cursor;
        }
        if (*cursor != L'\0') {
            ++g_debug_lines;
            H47VR_LOG_INFO("engine: %s", Narrow(cursor).c_str());
        }
        g_in_debug_output = false;
    }
    g_real_output_debug_string_w(text);
}

// SwapBuffers is presented from gdi32, not opengl32, so it is the presentation
// hook and the first place a real frame count can come from.
using SwapBuffersFn = BOOL(WINAPI*)(HDC);
SwapBuffersFn g_real_swap_buffers = nullptr;
std::atomic<unsigned long> g_presented_frames{0};

// Defined below; the presentation hook is where a module reload gets turned
// back into live hooks, because that is a frame boundary on the engine's thread.
void ApplyReinstall();

BOOL WINAPI HookSwapBuffers(HDC device) {
    const unsigned long frame = ++g_presented_frames;
    // Frame boundary: safe to patch code, and the only place a module reload
    // can be turned back into live hooks.
    ApplyReinstall();
    if (frame == 1) {
        // Safe place to patch renderer code: it is fully loaded and running by
        // the time the first frame is presented.
        RendererProbeInstall();
    }
    // The engine's own thread, once per frame: the only place the console
    // dispatcher can be re-entered safely.
    ConsoleDriverPump(frame);
    // The camera write has to be offered every frame, not on the report
    // heartbeat: the engine rewrites the camera from input, so a one-shot write
    // would be lost again immediately.
    CameraProbeFrame(frame);
    CameraReadbackFrame(frame);
    // Head pose arrives here: posting WM_MOUSEMOVE makes the engine rotate its
    // own camera, so its visibility pass follows and nothing is culled.
    LookDriverFrame(frame);
    // First few frames, then a heartbeat: enough to prove presentation is
    // intercepted and to estimate the rate without flooding the log.
    if (frame <= 5) {
        H47VR_LOG_INFO("presented frame %lu", frame);
    }
    if (frame % 120 == 0) {
        HooksFlushModuleEvents();
        RendererProbeReport(frame);
        GlProbeReport(frame);
        CameraProbeReport(frame);
        LookDriverReport(frame);
        if (WindowKeepaliveSwallowed() != 0) {
            H47VR_LOG_INFO("frame %lu window keepalive: %lu deactivation message(s) swallowed",
                           frame, WindowKeepaliveSwallowed());
        }
    }
    return g_real_swap_buffers(device);
}

// The gameplay entry points are exported with a leading underscore and no @N
// suffix, which is MSVC's cdecl decoration rather than stdcall. An unknown
// arity is therefore safe: the caller cleans the stack, so forwarding more
// arguments than the target uses changes nothing.
using GameplayFn = void(__cdecl*)(void*, void*, void*, void*, void*, void*, void*, void*);
GameplayFn g_real_mouse_con_cam = nullptr;
std::atomic<unsigned long> g_camera_calls{0};

void __cdecl HookMouseConCam(void* a, void* b, void* c, void* d,
                             void* e, void* f, void* g, void* h) {
    const unsigned long call = ++g_camera_calls;
    if (call <= 8 || call % 600 == 0) {
        H47VR_LOG_INFO("_ZCAMERA_MouseConCam #%lu  a=%p b=%p c=%p d=%p",
                       call, a, b, c, d);
    }
    g_real_mouse_con_cam(a, b, c, d, e, f, g, h);
}

// Runs once HitmanDlc.dlc appears, which is after the proxy has started.
void InstallGameplayHooks() {
    const HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    void* target = ResolveSymbol(L"HitmanDlc.dlc", "_ZCAMERA_MouseConCam");
    if (gameplay != nullptr && target != nullptr) {
        const auto offset = static_cast<unsigned long long>(
            reinterpret_cast<std::uintptr_t>(target) -
            reinterpret_cast<std::uintptr_t>(gameplay));
        H47VR_LOG_INFO("HitmanDlc.dlc base %p, camera entry rva 0x%06llX",
                       gameplay, offset);
    }
    HookSymbol(L"HitmanDlc.dlc", "_ZCAMERA_MouseConCam",
               reinterpret_cast<void*>(&HookMouseConCam),
               reinterpret_cast<void**>(&g_real_mouse_con_cam));
}

// Level transitions reload the engine's modules, and a reloaded module is fresh
// memory with no patches in it. Anything installed by address has to be
// re-installed, or the mod goes quiet for the rest of the session without
// saying why.
//
// The loader notification runs on a worker thread, and patching code that the
// render thread may be executing is not safe from there, so it only raises a
// flag: the work happens at the next frame boundary.
std::atomic<bool> g_need_reinstall{false};

void RequestReinstall() {
    g_need_reinstall = true;
}

void ApplyReinstall() {
    if (!g_need_reinstall.exchange(false)) {
        return;
    }

    const HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    const HMODULE renderer = LoadedModule(L"RenderOpenGL.dll");
    H47VR_LOG_INFO("re-arming: HitmanDlc.dlc base %p, RenderOpenGL.dll base %p",
                   gameplay, renderer);

    if (gameplay != nullptr) {
        InstallGameplayHooks();
        CameraProbeInstall();
        CameraReadbackRearm();
    }
    if (renderer != nullptr) {
        RendererProbeReset();
        RendererProbeInstall();
        LookDriverRearm();
    }
    if (LoadedModule(L"System.dll") != nullptr) {
        ConsoleDriverRearm();
    }
    if (LoadedModule(L"EngineData.dll") != nullptr) {
        SceneProbeRearm();
    }
}

void InstallRendererHooks() {
    HookSymbol(L"gdi32.dll", "SwapBuffers",
               reinterpret_cast<void*>(&HookSwapBuffers),
               reinterpret_cast<void**>(&g_real_swap_buffers));
}

void InstallConsoleCapture() {
    HookSymbol(L"kernel32.dll", "OutputDebugStringA",
               reinterpret_cast<void*>(&HookOutputDebugStringA),
               reinterpret_cast<void**>(&g_real_output_debug_string));
    HookSymbol(L"kernel32.dll", "OutputDebugStringW",
               reinterpret_cast<void*>(&HookOutputDebugStringW),
               reinterpret_cast<void**>(&g_real_output_debug_string_w));

}

}  // namespace

bool Initialize(const char* trigger) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_started) {
            return g_compatible;
        }
        g_started = true;
        g_gameDirectory = ExecutableDirectory();
    }

    const std::wstring logDirectory = JoinPath(g_gameDirectory, L"h47vr-logs");
    const bool logging = LogStart(logDirectory.c_str(), trigger != nullptr ? trigger : "h47vr");

    H47VR_LOG_INFO("h47vr %s starting, trigger: %s",
                   H47VR_VERSION_STRING, trigger != nullptr ? trigger : "(none)");
    if (!logging) {
        H47VR_LOG_WARN("no log file was opened; continuing without persistent logging");
    }

    H47VR_LOG_INFO("game directory: %s", Narrow(g_gameDirectory).c_str());
    LogSystemContext();

    const FingerprintReport report = VerifyInstall(g_gameDirectory);
    for (const PinCheck& check : report.checks) {
        if (check.ok) {
            H47VR_LOG_INFO("pin  ok   %s", check.relativePath.c_str());
        } else if (!check.present) {
            H47VR_LOG_ERROR("pin  miss %s", check.relativePath.c_str());
        } else {
            H47VR_LOG_ERROR("pin  fail %s (expected %s, got %s)",
                            check.relativePath.c_str(), check.expected.c_str(),
                            check.actual.c_str());
        }
    }

    if (!report.allMatch) {
        H47VR_LOG_ERROR(
            "unsupported build (appid %s buildid %s): %d missing, %d mismatched. "
            "No patches will be applied.",
            report.appId.c_str(), report.buildId.c_str(),
            report.MissingCount(), report.MismatchedCount());
        g_compatible = false;
        return false;
    }

    H47VR_LOG_INFO("supported build %s confirmed", report.buildId.c_str());

    LogSymbolProbes();

    HooksInitialize();
    // The engine stops drawing the moment it loses activation, which is what
    // made every automated run look like it had wedged. Start this before the
    // window exists so it wins the race to subclass it.
    WindowKeepaliveStart();
    InstallRendererHooks();
    InstallConsoleCapture();
    // The GL seam has to be in place before the renderer draws, so it is
    // installed here rather than at the first presented frame. The filter file
    // names the entries to intercept, so a misbehaving one can be pinned down
    // without a rebuild; h47vr-no-glprobe.txt is shorthand for "none".
    std::string glFilter = "all";
    const std::wstring glFilterPath = JoinPath(g_gameDirectory, L"h47vr-glprobe.txt");
    if (FileExists(glFilterPath)) {
        glFilter = FirstLine(glFilterPath);
    }
    if (FileExists(JoinPath(g_gameDirectory, L"h47vr-no-glprobe.txt"))) {
        glFilter = "none";
    }
    H47VR_LOG_INFO("gl probe: filter '%s'", glFilter.c_str());
    GlProbeInstall(glFilter.c_str());
    SceneProbeInstall();
    ConsoleDriverInstall();
    LookDriverStart();
    WatchModuleLoads(L"HitmanDlc.dlc", &RequestReinstall);
    WatchModuleLoads(L"RenderOpenGL.dll", &RequestReinstall);
    WatchModuleLoads(L"System.dll", &RequestReinstall);

    H47VR_LOG_INFO(
        "milestone 1: presentation and camera interception installed; no "
        "rendering or camera modification yet.");

    g_compatible = true;
    return true;
}

void Shutdown() {
    H47VR_LOG_INFO("h47vr shutting down");
    LogStop();

    std::lock_guard<std::mutex> lock(g_mutex);
    g_started = false;
    g_compatible = false;
}

bool IsRunning() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_started;
}

bool IsCompatible() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_compatible;
}

const std::wstring& GameDirectory() {
    return g_gameDirectory;
}

}  // namespace h47vr::mod
