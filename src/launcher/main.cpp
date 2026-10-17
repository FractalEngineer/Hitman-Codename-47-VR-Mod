// Development and diagnostic front end.
//
//   h47vr-launcher verify [gameDir]        check the build pins and exit
//   h47vr-launcher run    [gameDir]        launch the game (proxy loads the mod)
//   h47vr-launcher inject [dll] [gameDir]  launch with an injected mod module
//
// The release path is the app-local opengl32.dll proxy and needs none of this.

#include "h47vr/fingerprint.hpp"
#include "h47vr/paths.hpp"

#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 1;
constexpr int kExitUnsupported = 2;
constexpr int kExitFailure = 3;

void PrintUsage() {
    std::fputs(
        "h47vr-launcher - development and diagnostic front end\n"
        "\n"
        "  verify [gameDir]       verify the installed build against the pins\n"
        "  run    [gameDir]       launch the game with the proxy in place\n"
        "  inject [dll] [gameDir] launch and inject h47vr_mod.dll\n"
        "\n"
        "gameDir defaults to the directory holding this executable.\n",
        stdout);
}

std::wstring ResolveGameDirectory(const std::wstring& requested) {
    if (!requested.empty()) {
        return requested;
    }
    const std::wstring here = h47vr::ExecutableDirectory();
    if (h47vr::FileExists(h47vr::JoinPath(here, L"Hitman.Exe"))) {
        return here;
    }
    return here;
}

int Verify(const std::wstring& gameDirectory) {
    const h47vr::FingerprintReport report = h47vr::VerifyInstall(gameDirectory);
    std::fputs("game directory: ", stdout);
    std::fputs(h47vr::Narrow(gameDirectory).c_str(), stdout);
    std::fputs("\n", stdout);
    const std::string text = h47vr::FormatReport(report);
    std::fputs(text.c_str(), stdout);
    return report.allMatch ? kExitOk : kExitUnsupported;
}

bool InjectModule(HANDLE process, const std::wstring& dllPath) {
    const std::size_t bytes = (dllPath.size() + 1) * sizeof(wchar_t);
    void* remote = ::VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT | MEM_RESERVE,
                                    PAGE_READWRITE);
    if (remote == nullptr) {
        std::fprintf(stderr, "VirtualAllocEx failed (error %lu)\n", ::GetLastError());
        return false;
    }

    bool ok = false;
    do {
        if (!::WriteProcessMemory(process, remote, dllPath.c_str(), bytes, nullptr)) {
            std::fprintf(stderr, "WriteProcessMemory failed (error %lu)\n", ::GetLastError());
            break;
        }

        HMODULE kernel32 = ::GetModuleHandleW(L"kernel32.dll");
        auto loadLibrary = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            ::GetProcAddress(kernel32, "LoadLibraryW"));
        if (loadLibrary == nullptr) {
            std::fputs("kernel32!LoadLibraryW was not found\n", stderr);
            break;
        }

        HANDLE thread = ::CreateRemoteThread(process, nullptr, 0, loadLibrary, remote, 0, nullptr);
        if (thread == nullptr) {
            std::fprintf(stderr, "CreateRemoteThread failed (error %lu)\n", ::GetLastError());
            break;
        }

        ::WaitForSingleObject(thread, 15000);
        DWORD result = 0;
        ::GetExitCodeThread(thread, &result);
        ::CloseHandle(thread);

        // A loaded module image is always mapped above 64 KB.
        if (result < 0x10000) {
            std::fprintf(stderr, "LoadLibraryW returned 0x%08lX; injection failed\n", result);
            break;
        }
        ok = true;
    } while (false);

    ::VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

int Launch(const std::wstring& gameDirectory, const std::wstring& injectDll) {
    const std::wstring exe = h47vr::JoinPath(gameDirectory, L"Hitman.Exe");
    if (!h47vr::FileExists(exe)) {
        std::fprintf(stderr, "Hitman.Exe was not found in %s\n",
                     h47vr::Narrow(gameDirectory).c_str());
        return kExitFailure;
    }

    std::wstring commandLine = L"\"" + exe + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    const DWORD flags = injectDll.empty() ? 0 : CREATE_SUSPENDED;
    if (!::CreateProcessW(exe.c_str(), commandLine.data(), nullptr, nullptr, FALSE, flags,
                          nullptr, gameDirectory.c_str(), &startup, &process)) {
        std::fprintf(stderr, "CreateProcessW failed (error %lu)\n", ::GetLastError());
        return kExitFailure;
    }

    std::fputs("launched ", stdout);
    std::fputs(h47vr::Narrow(exe).c_str(), stdout);
    std::fputs("\n", stdout);

    if (!injectDll.empty()) {
        if (!InjectModule(process.hProcess, injectDll)) {
            std::fputs("injection failed; terminating\n", stderr);
            ::TerminateProcess(process.hProcess, kExitFailure);
            ::CloseHandle(process.hThread);
            ::CloseHandle(process.hProcess);
            return kExitFailure;
        }
        std::fputs("injected ", stdout);
        std::fputs(h47vr::Narrow(injectDll).c_str(), stdout);
        std::fputs("\n", stdout);
        ::ResumeThread(process.hThread);
    }

    ::WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);

    std::fprintf(stdout, "game exited with code %lu\n", exitCode);
    return kExitOk;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);

    if (argc < 2) {
        PrintUsage();
        return kExitUsage;
    }

    const std::wstring command = argv[1];

    if (command == L"verify") {
        return Verify(ResolveGameDirectory(argc > 2 ? argv[2] : L""));
    }

    if (command == L"run") {
        return Launch(ResolveGameDirectory(argc > 2 ? argv[2] : L""), L"");
    }

    if (command == L"inject") {
        std::wstring dll = argc > 2 ? argv[2] : L"h47vr_mod.dll";
        if (dll.find(L'\\') == std::wstring::npos) {
            dll = h47vr::JoinPath(h47vr::ExecutableDirectory(), dll);
        }
        return Launch(ResolveGameDirectory(argc > 3 ? argv[3] : L""), dll);
    }

    if (command == L"--help" || command == L"-h" || command == L"help") {
        PrintUsage();
        return kExitOk;
    }

    PrintUsage();
    return kExitUsage;
}
