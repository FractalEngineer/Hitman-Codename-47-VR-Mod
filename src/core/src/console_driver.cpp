#include "h47vr/console_driver.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/renderer_probe.hpp"
#include "h47vr/scene_probe.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

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
// cleanup (`ret 8`). The ABI came from the call site at RVA 0x104AD and the
// prologue, not from the decompiler's guess.
constexpr unsigned long kCommandDispatcherRva = 0x0000E590;

// Globals.dll exports the interface singleton the dispatcher expects in ECX.
constexpr unsigned long kSysInterfaceRva = 0x00003010;
constexpr const char* kSysInterfaceSymbol = "?g_pSysInterface@@3PAVZSysInterface@@A";

// The dispatcher takes the command name and its argument separately, having
// split the line at the first '=' or space.
constexpr const char* kStartSceneCommand = "startscene";

// The dispatcher loads `mov ecx, [edi+0x59]` for the world object it hands the
// scene name to, and `call [eax+0x44]` on that object's vtable.
constexpr int kWorldFieldOffset = 0x59;
constexpr int kWorldLoadSlot = 0x44 / 4;

// The scheduled path is a fallback: it fires from start-up, which lands during
// the intro videos, so the on-demand trigger is the reliable one.
constexpr unsigned long kFirstAttemptFrame = 240;
constexpr unsigned long kRetryFrames = 300;
constexpr unsigned int kMaxAttempts = 8;

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

// Candidates for the scheduled path, tried in order, one per attempt.
std::vector<std::string> g_scenes;
unsigned int g_attempts = 0;
unsigned long g_next_attempt_frame = kFirstAttemptFrame;
bool g_reported_load = false;
bool g_logged_loader = false;

// The interface singleton lives behind an exported data slot, so resolving the
// symbol gives us the slot and we still have to read it.
void* InterfaceObject() {
    auto* slot = reinterpret_cast<void**>(ResolveSymbol(L"Globals.dll", kSysInterfaceSymbol));
    if (slot == nullptr) {
        return nullptr;
    }
    return *slot;
}

// First non-empty, non-comment line. A hash mark starts a comment.
std::string ReadFirstLine(const std::wstring& path) {
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
        return line;
    }
    return {};
}

std::vector<std::string> ReadSceneRequest() {
    std::vector<std::string> scenes;
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
            scenes.push_back(line);
        }
    }

    if (!scenes.empty()) {
        return scenes;
    }

    char buffer[MAX_PATH]{};
    const DWORD length = ::GetEnvironmentVariableA("H47VR_SCENE", buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        scenes.push_back(std::string(buffer, length));
    }
    return scenes;
}

// The on-demand trigger. Its appearance means "submit this now": only a player
// can tell when the front end has actually been reached, because the intro
// videos have to be skipped by hand. Consumed on sight.
std::string TakeGoRequest() {
    const std::wstring path = JoinPath(mod::GameDirectory(), L"h47vr-go.txt");
    if (!FileExists(path)) {
        return {};
    }
    const std::string payload = ReadFirstLine(path);
    if (!::DeleteFileW(path.c_str())) {
        H47VR_LOG_WARN("console driver: could not remove %s (error %lu)",
                       Narrow(path).c_str(), ::GetLastError());
    }
    return payload;
}

// Every console or config line the engine executes lands here, so a typed
// command shows up in the log with the exact name and argument the parser
// produced.
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

// Loads a level through the same vtable slot the PreScene screen uses. This is
// the game's own level start, so it is the one path that is known to work.
void SubmitPushLevel(const std::string& level, unsigned long frame) {
    void* self = InterfaceObject();
    if (self == nullptr) {
        H47VR_LOG_WARN("scene push: g_pSysInterface is still null (frame %lu)", frame);
        return;
    }
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + kWorldFieldOffset);
    if (world == nullptr) {
        H47VR_LOG_WARN("scene push: the world object at +0x%X is still null (frame %lu)",
                       kWorldFieldOffset, frame);
        return;
    }

    const int result = ScenePushLevel(world, level.c_str());
    H47VR_LOG_INFO("console driver: scene push '%s' -> %d (frame %lu, world=%p)",
                   level.c_str(), result, frame, world);
}

// Calls the scene entry the way the engine itself does, with a mode we choose,
// and keeps the result. This is the experiment harness: the console command
// hard-codes mode 2 and discards the answer.
void SubmitSceneEntry(int mode, const std::string& name, unsigned long frame) {
    void* self = InterfaceObject();
    if (self == nullptr) {
        H47VR_LOG_WARN("scene entry: g_pSysInterface is still null (frame %lu)", frame);
        return;
    }
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + kWorldFieldOffset);
    if (world == nullptr) {
        H47VR_LOG_WARN("scene entry: the world object at +0x%X is still null (frame %lu)",
                       kWorldFieldOffset, frame);
        return;
    }

    const int result = SceneEntryInvoke(world, mode, name.c_str(), nullptr);
    H47VR_LOG_INFO("console driver: scene entry mode=%d name='%s' -> %d (frame %lu, world=%p)",
                   mode, name.c_str(), result, frame, world);
}

void SubmitCommandLine(const std::string& line, unsigned long frame) {
    void* self = InterfaceObject();
    if (self == nullptr) {
        H47VR_LOG_WARN("console command: g_pSysInterface is still null (frame %lu)",
                       frame);
        return;
    }
    const std::size_t split = line.find_first_of("= ");
    const std::string name = line.substr(0, split);
    const std::string argument =
        split == std::string::npos ? std::string() : line.substr(split + 1);
    if (name.empty()) {
        H47VR_LOG_WARN("console command: empty command line");
        return;
    }
    h47vr_command_self = self;
    const int result =
        h47vr_invoke_command(name.c_str(), argument.c_str());
    H47VR_LOG_INFO("console command: '%s' arg='%s' -> %d (frame %lu)",
                   name.c_str(), argument.c_str(), result, frame);
}

void SubmitScene(const std::string& scene, const char* why, unsigned long frame) {
    void* self = InterfaceObject();
    if (self == nullptr) {
        H47VR_LOG_WARN("console driver: g_pSysInterface is still null (%s, frame %lu)",
                       why, frame);
        return;
    }

    // The dispatcher hands the scene name to `(*(self+0x59))->vtbl[0x44]` and
    // discards that call's return value, so log where the virtual lives once:
    // otherwise a failed load and a successful one look identical.
    void* world = *reinterpret_cast<void**>(reinterpret_cast<char*>(self) + kWorldFieldOffset);
    if (!g_logged_loader && world != nullptr) {
        g_logged_loader = true;
        void** vtable = *reinterpret_cast<void***>(world);
        auto* loader = vtable[kWorldLoadSlot];
        unsigned long long offset = 0;
        if (ModuleOffset(loader, L"HitmanDlc.dlc", offset)) {
            H47VR_LOG_INFO("console driver: world %p vtbl %p, load virtual %p = HitmanDlc.dlc "
                           "rva 0x%06llX",
                           world, vtable, loader, offset);
        } else {
            H47VR_LOG_INFO("console driver: world %p vtbl %p, load virtual %p in %s",
                           world, vtable, loader,
                           Narrow(ModuleContaining(loader)).c_str());
        }
    }

    h47vr_command_self = self;
    SceneProbeMarkOwnCall();
    const int result = h47vr_invoke_command(kStartSceneCommand, scene.c_str());
    H47VR_LOG_INFO(
        "console driver: 'startscene %s' -> %d (%s, attempt %u, frame %lu, self=%p, world=%p)",
        scene.c_str(), result, why, g_attempts, frame, self, world);
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
    g_scenes = ReadSceneRequest();

    if (!g_scenes.empty()) {
        H47VR_LOG_INFO("console driver: %u scheduled candidate(s), first attempt at frame %lu",
                       static_cast<unsigned>(g_scenes.size()), kFirstAttemptFrame);
        for (const std::string& scene : g_scenes) {
            H47VR_LOG_INFO("  candidate: '%s'", scene.c_str());
        }
    } else {
        H47VR_LOG_INFO("console driver: nothing scheduled; write h47vr-scene.txt or "
                       "h47vr-go.txt to submit a scene");
    }

    InstallHook();
    return g_hooked.load();
}

bool ConsoleDriverRearm() {
    InstallHook();
    return g_hooked.load();
}

void ConsoleDriverPump(unsigned long frame) {
    if (h47vr_command_trampoline == nullptr) {
        return;
    }

    // On-demand trigger first. Checking for the file is cheap, but there is no
    // need to do it sixty times a second either.
    if (frame % 15 == 0) {
        const std::string go = TakeGoRequest();
        if (!go.empty() && go != "none" && go != "off") {
            ++g_attempts;
            RendererProbeReportState("before submit");
            // "push:<level>" is the game's own level start.
            if (go.rfind("push:", 0) == 0) {
                SubmitPushLevel(go.substr(5), frame);
                return;
            }
            if (go.rfind("cmd:", 0) == 0) {
                SubmitCommandLine(go.substr(4), frame);
                return;
            }
            // "<mode>:<name>" calls the scene entry directly with that mode,
            // which is how the engine's own level start gets replayed. Anything
            // else goes through the console command as before.
            if (go.size() > 2 && go[1] == ':' && go[0] >= '0' && go[0] <= '9') {
                SubmitSceneEntry(go[0] - '0', go.substr(2), frame);
            } else {
                SubmitScene(go, "on demand", frame);
            }
            return;
        }
    }

    // World geometry drawing means a level is up. Scheduled scene requests are
    // only for start-up; on-demand commands above remain available afterwards.
    if (h47vr_render_counts[2] != 0) {
        if (!g_reported_load) {
            g_reported_load = true;
            H47VR_LOG_INFO("console driver: world geometry is rendering");
        }
        return;
    }

    if (g_scenes.empty()) {
        return;
    }
    if (g_attempts >= kMaxAttempts || frame < g_next_attempt_frame) {
        return;
    }
    g_next_attempt_frame = frame + kRetryFrames;
    ++g_attempts;
    SubmitScene(g_scenes[(g_attempts - 1) % g_scenes.size()], "scheduled", frame);
}

}  // namespace h47vr
