#include "h47vr/console_driver.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>

// Defined by renderer_probe.cpp. Entry 2 counts world-geometry draws
// (glDrawElements with a model matrix), which stay at zero until a level is
// actually rendering.
extern "C" unsigned long h47vr_render_counts[4];

namespace h47vr {

// Logs every line the engine submits to the console/config dispatcher. Defined
// below, outside the unnamed namespace, so the detour thunk can reach it
// through h47vr_observe_target.
void ObserveCommand(void* self, const char* name, const char* arg);

namespace {

// System.dll's command dispatcher: __thiscall, two stack arguments, callee
// cleanup (`ret 8`). We verified the ABI from the call site at RVA 0x104AD and
// the prologue, rather than inferring it from the decompiler's guess.
constexpr unsigned long kCommandDispatcherRva = 0x0000E590;

// Globals.dll exports the interface singleton the dispatcher expects in ECX.
constexpr unsigned long kSysInterfaceRva = 0x00003010;
constexpr const char* kSysInterfaceSymbol = "?g_pSysInterface@@3PAVZSysInterface@@A";

// The dispatcher only recognises the argument separator as the first '=' or
// space in a line, so the name and the argument are passed separately.
constexpr const char* kStartSceneCommand = "startscene";

// The dispatcher loads `mov ecx, [edi+0x59]` for the world object it hands the
// scene name to, so that really is the field offset.
constexpr int kWorldFieldOffset = 0x59;

// The intro videos and the menu take a few seconds to settle before a scene
// load is accepted. Wait, then retry on a slow cadence until geometry appears.
constexpr unsigned long kFirstAttemptFrame = 300;
constexpr unsigned long kRetryFrames = 600;
constexpr unsigned int kMaxAttempts = 4;

// The dispatcher is a __thiscall (`ret 8`): ECX carries the interface object and
// the callee cleans both stack arguments. MSVC only allows __thiscall on member
// functions, so both directions go through naked thunks that set or preserve
// ECX and let the original `ret 8` do the cleanup.
extern "C" void* h47vr_command_trampoline = nullptr;
extern "C" void* h47vr_command_self = nullptr;

// The thunk reaches the observer through a pointer so the observer itself can
// stay an ordinary C++ function instead of a hand-mangled asm symbol.
extern "C" void* h47vr_observe_target = nullptr;

std::atomic<bool> g_hooked{false};
std::atomic<unsigned long> g_command_lines{0};
std::atomic<void*> g_last_self{nullptr};

std::string g_scene;
bool g_have_scene = false;
unsigned int g_attempts = 0;
unsigned long g_next_attempt_frame = kFirstAttemptFrame;
bool g_reported_load = false;

// The interface singleton lives behind an exported data slot, so resolving the
// symbol gives us the slot and we still have to read it.
void* InterfaceObject() {
    auto* slot = reinterpret_cast<void**>(ResolveSymbol(L"Globals.dll", kSysInterfaceSymbol));
    if (slot == nullptr) {
        return nullptr;
    }
    return *slot;
}

std::string ReadSceneRequest() {
    const std::wstring path = JoinPath(mod::GameDirectory(), L"h47vr-scene.txt");
    if (FileExists(path)) {
        std::ifstream file(path);
        std::string line;
        while (std::getline(file, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n' ||
                                     line.back() == ' ' || line.back() == '\t')) {
                line.pop_back();
            }
            const auto first = line.find_first_not_of(" \t");
            if (first == std::string::npos || line[first] == '#') {
                continue;
            }
            line.erase(0, first);
            if (line == "none" || line == "off") {
                return {};
            }
            return line;
        }
    }

    char buffer[MAX_PATH]{};
    const DWORD length = ::GetEnvironmentVariableA("H47VR_SCENE", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        return std::string(buffer, length);
    }
    return {};
}

// Every console or config line the engine executes lands here, so a typed
// command shows up in the log with the exact name and argument the parser
// produced - which is the ground truth the handoff was missing.
//
// Layout on entry to the thunk: [esp]=return address, [esp+4]=name,
// [esp+8]=argument, ECX=interface object.
extern "C" __declspec(naked) int __stdcall h47vr_detour_command(const char*, const char*) {
    __asm {
        pushfd
        pushad
        // After pushfd+pushad the saved registers start at esp: ecx is at
        // [esp+24], the return address at [esp+36], name at [esp+40] and the
        // argument at [esp+44].
        push dword ptr [esp+44]     // argument
        push dword ptr [esp+44]     // name (offset is unchanged by one push)
        push dword ptr [esp+32]     // interface object, saved from ecx
        call dword ptr [h47vr_observe_target]
        add esp, 12
        popad
        popfd
        // Tail-jump to the original bytes: ecx and the stack arguments are
        // exactly as the engine left them, and the original `ret 8` cleans up.
        jmp dword ptr [h47vr_command_trampoline]
    }
}

// Calls the dispatcher the same way the engine does, for a line the mod
// synthesises. __stdcall because the target cleans the arguments itself.
extern "C" __declspec(naked) int __stdcall h47vr_invoke_command(const char*, const char*) {
    __asm {
        mov ecx, dword ptr [h47vr_command_self]
        jmp dword ptr [h47vr_command_trampoline]
    }
}

void InstallHook() {
    HMODULE system = LoadedModule(L"System.dll");
    if (system == nullptr) {
        H47VR_LOG_WARN("console driver: System.dll is not loaded");
        return;
    }

    h47vr_observe_target = reinterpret_cast<void*>(&ObserveCommand);

    // The dispatcher takes the interface singleton in ECX; confirm the exported
    // slot really is where the hook map says it is.
    HMODULE globals = LoadedModule(L"Globals.dll");
    const auto slot = reinterpret_cast<std::uintptr_t>(
        ResolveSymbol(L"Globals.dll", kSysInterfaceSymbol));
    if (globals != nullptr && slot != 0) {
        H47VR_LOG_INFO("console driver: g_pSysInterface slot rva 0x%06llX (expected 0x%06lX)",
                       static_cast<unsigned long long>(slot -
                           reinterpret_cast<std::uintptr_t>(globals)),
                       kSysInterfaceRva);
    }

    void* target = reinterpret_cast<char*>(system) + kCommandDispatcherRva;
    if (!HookAddress(target, reinterpret_cast<void*>(&h47vr_detour_command),
                     &h47vr_command_trampoline)) {
        H47VR_LOG_ERROR("console driver: could not hook the command dispatcher at %p", target);
        return;
    }
    g_hooked = true;
    H47VR_LOG_INFO("hooked System.dll command dispatcher rva 0x%06lX at %p",
                   kCommandDispatcherRva, target);
}

}  // namespace

void ObserveCommand(void* self, const char* name, const char* arg) {
    const unsigned long line = ++g_command_lines;
    g_last_self = self;
    if (line <= 32 || line % 200 == 0) {
        H47VR_LOG_INFO("console line #%lu: name='%s' arg='%s' self=%p",
                       line,
                       name != nullptr ? name : "(null)",
                       arg != nullptr ? arg : "(null)",
                       self);
    }
}

bool ConsoleDriverInstall() {
    g_scene = ReadSceneRequest();
    g_have_scene = !g_scene.empty();

    if (g_have_scene) {
        H47VR_LOG_INFO("console driver: scene request '%s' (first attempt at frame %lu)",
                       g_scene.c_str(), kFirstAttemptFrame);
    } else {
        H47VR_LOG_INFO("console driver: no scene requested "
                       "(write h47vr-scene.txt or set H47VR_SCENE to enable)");
    }

    InstallHook();
    return g_hooked.load();
}

void ConsoleDriverPump(unsigned long frame) {
    if (!g_have_scene || h47vr_command_trampoline == nullptr) {
        return;
    }

    // World geometry drawing means a level is up and the camera hook is about
    // to matter; that is the signal the whole milestone was waiting on.
    if (h47vr_render_counts[2] != 0) {
        if (!g_reported_load) {
            g_reported_load = true;
            H47VR_LOG_INFO("console driver: world geometry is rendering - '%s' loaded",
                           g_scene.c_str());
        }
        return;
    }

    if (g_attempts >= kMaxAttempts || frame < g_next_attempt_frame) {
        return;
    }
    g_next_attempt_frame = frame + kRetryFrames;
    ++g_attempts;

    void* self = InterfaceObject();
    if (self == nullptr) {
        H47VR_LOG_WARN("console driver: g_pSysInterface is still null (attempt %u, frame %lu)",
                       g_attempts, frame);
        return;
    }

    h47vr_command_self = self;
    // The startscene branch refuses to do anything unless the world object at
    // the interface's +0x59 is already constructed, and it reports that as a
    // plain 0 with no message. Log it so a refused attempt is diagnosable.
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + kWorldFieldOffset);
    const int result = h47vr_invoke_command(kStartSceneCommand, g_scene.c_str());
    H47VR_LOG_INFO("console driver: 'startscene %s' -> %d (attempt %u, frame %lu, self=%p, world=%p)",
                   g_scene.c_str(), result, g_attempts, frame, self, world);
}

}  // namespace h47vr
