#include "h47vr/mod.hpp"

#include "h47vr/fingerprint.hpp"
#include "h47vr/log.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/version.hpp"

#include <windows.h>

#include <cstdio>
#include <mutex>

namespace h47vr::mod {
namespace {

std::mutex g_mutex;
bool g_started = false;
bool g_compatible = false;
std::wstring g_gameDirectory;

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
    H47VR_LOG_INFO(
        "milestone 1: hook installation is not implemented yet; the mod is "
        "currently a verified loader with diagnostics only.");

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
