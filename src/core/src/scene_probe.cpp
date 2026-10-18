#include "h47vr/scene_probe.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <atomic>

namespace h47vr {
namespace {

// `(*(interface+0x59))->vtbl[0x44]`, the entry the console `startscene` command
// calls. EngineData.dll's preferred base is 0xFF60000, so this is RVA 0xEF60.
constexpr unsigned long kSceneEntryRva = 0x0000EF60;

// Register-preserving thunk: log, then continue into the original code with
// everything exactly as the engine left it.
extern "C" void* h47vr_scene_trampoline = nullptr;

// The thunk calls its observers through pointers, so they can stay ordinary C++
// functions instead of hand-mangled asm symbols.
extern "C" void* h47vr_scene_args_target = nullptr;

std::atomic<bool> g_own_call{false};
std::atomic<unsigned long> g_calls{0};

// Entry layout: [esp]=return address, [esp+4]=mode, [esp+8]=argument,
// [esp+12]=second argument, ECX=object. Ghidra reads this as
// __thiscall(void*, int, char*, char*); the thunk does not depend on that.
extern "C" __declspec(naked) void h47vr_detour_scene_entry() {
    __asm {
        pushfd
        pushad
        // After pushfd+pushad: ecx is at [esp+24], eflags at [esp+32], the
        // return address at [esp+36], and the three arguments at [esp+40],
        // [esp+44] and [esp+48].
        push dword ptr [esp+48]     // second argument
        push dword ptr [esp+48]     // first argument
        push dword ptr [esp+48]     // mode
        push dword ptr [esp+36]     // object, saved from ecx
        call dword ptr [h47vr_scene_args_target]
        add esp, 16
        popad
        popfd
        jmp dword ptr [h47vr_scene_trampoline]
    }
}

void ObserveSceneEntry(void* object, int mode, const char* first, const char* second) {
    const unsigned long call = ++g_calls;
    const bool own = g_own_call.exchange(false);
    // The engine calls this constantly for its own purposes, so log the mod's
    // own requests unconditionally and only the first handful of engine calls.
    if (own || call <= 12 || call % 200 == 0) {
        H47VR_LOG_INFO("scene entry #%lu%s: object=%p mode=%d arg1='%s' arg2='%s'",
                       call, own ? " (from the mod)" : "",
                       object, mode,
                       first != nullptr ? first : "(null)",
                       second != nullptr ? second : "(null)");
    }
}

}  // namespace

bool SceneProbeInstall() {
    HMODULE engine = LoadedModule(L"EngineData.dll");
    if (engine == nullptr) {
        H47VR_LOG_WARN("scene probe: EngineData.dll is not loaded");
        return false;
    }

    h47vr_scene_args_target = reinterpret_cast<void*>(&ObserveSceneEntry);

    void* target = reinterpret_cast<char*>(engine) + kSceneEntryRva;
    if (!HookAddress(target, reinterpret_cast<void*>(&h47vr_detour_scene_entry),
                     &h47vr_scene_trampoline)) {
        H47VR_LOG_ERROR("scene probe: could not hook EngineData.dll rva 0x%06lX at %p",
                        kSceneEntryRva, target);
        return false;
    }

    unsigned long long offset = 0;
    H47VR_LOG_INFO("hooked scene entry EngineData.dll rva 0x%06lX at %p (%s)",
                   kSceneEntryRva, target,
                   ModuleOffset(target, L"EngineData.dll", offset) ? "in EngineData" : "elsewhere");
    return true;
}

bool SceneProbeRearm() {
    return SceneProbeInstall();
}

void SceneProbeMarkOwnCall() {
    g_own_call = true;
}

}  // namespace h47vr
