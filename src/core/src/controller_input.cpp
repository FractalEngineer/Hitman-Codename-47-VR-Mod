#include "h47vr/controller_input.hpp"

#include "h47vr/camera_readback.hpp"
#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"
#include "h47vr/window_keepalive.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace h47vr {
namespace {

constexpr float kStickActivate = 0.32f;

std::mutex g_mutex;
ControllerInputSnapshot g_physical;
ControllerInputSnapshot g_debug;
std::atomic<unsigned long long> g_escape_generation_seen{0};
bool g_debug_active = false;
bool g_installed = false;
std::atomic<unsigned long> g_synthetic_keys{0};
std::wstring g_test_path;

using GetAsyncKeyStateFn = SHORT(WINAPI*)(int);
GetAsyncKeyStateFn g_real_get_async_key_state = nullptr;

float ClampAxis(float value) {
    if (!std::isfinite(value)) {
        return 0.0f;
    }
    return std::clamp(value, -1.0f, 1.0f);
}

bool AxisDown(float value) {
    return std::fabs(value) >= kStickActivate;
}

bool ReadNumber(const std::string& line, const char* key, float* value) {
    const std::string marker = std::string(key) + "=";
    const std::size_t at = line.find(marker);
    if (at == std::string::npos) {
        return false;
    }
    const char* text = line.c_str() + at + marker.size();
    char* end = nullptr;
    const float parsed = std::strtof(text, &end);
    if (end == text || parsed != parsed) {
        return false;
    }
    *value = parsed;
    return true;
}

std::string ReadFirstLine(const std::wstring& path) {
    std::ifstream file(path);
    std::string line;
    while (std::getline(file, line)) {
        while (!line.empty() &&
               (line.back() == '\r' || line.back() == '\n' ||
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

void RefreshDebugOverride() {
    const bool present =
        !g_test_path.empty() &&
        ::GetFileAttributesW(g_test_path.c_str()) != INVALID_FILE_ATTRIBUTES;

    ControllerInputSnapshot next;
    if (present) {
        const std::string line = ReadFirstLine(g_test_path);
        next.valid = true;
        ReadNumber(line, "lx", &next.leftX);
        ReadNumber(line, "ly", &next.leftY);
        ReadNumber(line, "rx", &next.rightX);
        ReadNumber(line, "ry", &next.rightY);
        float escape = 0.0f;
        float snap = 0.0f;
        ReadNumber(line, "esc", &escape);
        ReadNumber(line, "snap", &snap);
        next.escapeDown = escape != 0.0f;
        next.snapDown = snap != 0.0f;
        next.leftX = ClampAxis(next.leftX);
        next.leftY = ClampAxis(next.leftY);
        next.rightX = ClampAxis(next.rightX);
        next.rightY = ClampAxis(next.rightY);
        next.leftActive = AxisDown(next.leftX) || AxisDown(next.leftY);
        next.rightActive = AxisDown(next.rightX) || AxisDown(next.rightY);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    if (next.escapeDown && !g_debug.escapeDown) {
        next.escapeGeneration = g_debug.escapeGeneration + 1;
    } else {
        next.escapeGeneration = g_debug.escapeGeneration;
    }
    if (next.snapDown && !g_debug.snapDown) {
        next.snapGeneration = g_debug.snapGeneration + 1;
    } else {
        next.snapGeneration = g_debug.snapGeneration;
    }
    if (present != g_debug_active) {
        g_debug_active = present;
        H47VR_LOG_INFO("controller input: headless test override %s",
                       present ? "enabled" : "disabled");
    }
    g_debug = next;
}

int MappedKeys(const ControllerInputSnapshot& state, int keys[5]) {
    keys[0] = keys[1] = keys[2] = keys[3] = keys[4] = 0;
    if (!state.valid) {
        return 0;
    }

    const bool gameplay = CameraReadbackGameplayActive();
    float x = 0.0f;
    float y = 0.0f;
    if (gameplay) {
        x = state.leftX;
        y = state.leftY;
    } else if (std::fabs(state.leftX) + std::fabs(state.leftY) >=
               std::fabs(state.rightX) + std::fabs(state.rightY)) {
        x = state.leftX;
        y = state.leftY;
    } else {
        x = state.rightX;
        y = state.rightY;
    }

    int count = 0;
    if (gameplay) {
        if (y < -kStickActivate) keys[count++] = 'S';
        if (y > kStickActivate) keys[count++] = 'W';
        if (x < -kStickActivate) keys[count++] = 'D';
        if (x > kStickActivate) keys[count++] = 'A';
    } else {
        if (y < -kStickActivate) keys[count++] = VK_DOWN;
        if (y > kStickActivate) keys[count++] = VK_UP;
        if (x < -kStickActivate) keys[count++] = VK_RIGHT;
        if (x > kStickActivate) keys[count++] = VK_LEFT;
    }
    return count;
}

bool SyntheticDown(int vk, bool* edge) {
    ControllerInputSnapshot state;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        state = g_debug_active ? g_debug : g_physical;
    }
    if (!state.valid) {
        return false;
    }

    if (vk == VK_ESCAPE) {
        if (!state.escapeDown) {
            return false;
        }
        *edge = state.escapeGeneration != 0 &&
                state.escapeGeneration != g_escape_generation_seen.load();
        if (*edge) {
            g_escape_generation_seen = state.escapeGeneration;
        }
        return true;
    }
    int keys[5] = {};
    const int count = MappedKeys(state, keys);
    for (int i = 0; i < count; ++i) {
        if (vk == keys[i]) {
            *edge = false;
            return true;
        }
    }
    return false;
}

void ApplyDirectKeyState(const ControllerInputSnapshot& state) {
    void* slot = ResolveSymbol(
        L"Globals.dll", "?g_pSysInterface@@3PAVZSysInterface@@A");
    if (slot == nullptr) {
        return;
    }
    auto* sysInterface = *reinterpret_cast<unsigned char**>(slot);
    if (sysInterface == nullptr) {
        return;
    }

    int keys[5] = {};
    const int count = MappedKeys(state, keys);
    static int previous[5] = {};
    static bool previousEscape = false;
    static unsigned long directWrites = 0;
    bool changed = false;
    const auto setKey = [&](int vk, bool down, bool edge) {
        if (vk <= 0) {
            return;
        }
        auto* keyState = sysInterface + 0xab7 + vk * 0xf;
        auto* keyEdge = sysInterface + 0xab6 + vk * 0xf;
        if (down) {
            if (*keyState == 0) {
                *keyState = 1;
                changed = true;
            }
            if (edge) {
                *keyEdge = 1;
                changed = true;
            } else if (*keyEdge != 0) {
                *keyEdge = 0;
            }
        } else if (*keyState != 0) {
            *keyState = 0;
            *keyEdge = 0;
            changed = true;
        }
    };

    for (int i = 0; i < 5; ++i) {
        bool down = false;
        for (int n = 0; n < count; ++n) {
            down = down || previous[i] == keys[n];
        }
        if (!down) {
            setKey(previous[i], false, false);
        }
    }
    for (int n = 0; n < count; ++n) {
        bool edge = true;
        for (int i = 0; i < 5; ++i) {
            edge = edge && previous[i] != keys[n];
        }
        setKey(keys[n], true, edge);
    }
    const bool escape = state.valid && state.escapeDown;
    const bool escapeEdge = escape && !previousEscape;
    setKey(VK_ESCAPE, escape, escapeEdge);
    for (int i = 0; i < 5; ++i) {
        previous[i] = i < count ? keys[i] : 0;
    }
    previousEscape = escape;
    if (changed && directWrites++ == 0) {
        H47VR_LOG_INFO("controller input: writing the engine's native "
                       "per-key state arrays");
    }
}

void PostKeyboardKey(int vk, bool down) {
    if (vk <= 0) {
        return;
    }
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    if (window == nullptr) {
        return;
    }
    const UINT scan = ::MapVirtualKeyW(static_cast<UINT>(vk),
                                       MAPVK_VK_TO_VSC);
    LPARAM lparam = MAKELPARAM(1, static_cast<WORD>(scan));
    if (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN) {
        lparam |= 0x01000000;
    }
    if (!down) {
        lparam |= static_cast<LPARAM>(0xC0000000u);
    }
    ::PostMessageW(window, down ? WM_KEYDOWN : WM_KEYUP,
                   static_cast<WPARAM>(vk), lparam);
}

void ApplyWindowKeyState(const ControllerInputSnapshot& state) {
    int keys[5] = {};
    const int count = MappedKeys(state, keys);
    static int previous[5] = {};
    static unsigned long transitions = 0;

    for (int i = 0; i < 5; ++i) {
        bool stillDown = false;
        for (int n = 0; n < count; ++n) {
            stillDown = stillDown || previous[i] == keys[n];
        }
        if (!stillDown && previous[i] != 0) {
            PostKeyboardKey(previous[i], false);
            ++transitions;
        }
    }
    for (int n = 0; n < count; ++n) {
        bool wasDown = false;
        for (int i = 0; i < 5; ++i) {
            wasDown = wasDown || previous[i] == keys[n];
        }
        if (!wasDown) {
            PostKeyboardKey(keys[n], true);
            ++transitions;
        }
    }
    for (int i = 0; i < 5; ++i) {
        previous[i] = i < count ? keys[i] : 0;
    }
    if (transitions == 1) {
        H47VR_LOG_INFO("controller input: left-stick movement is posting "
                       "engine keyboard messages");
    }
}

struct PendingKeyRelease {
    int vk;
    ULONGLONG tick;
};
PendingKeyRelease g_pendingReleases[8] = {};
int g_pendingReleaseCount = 0;

void ProcessPendingKeyReleases() {
    const ULONGLONG now = ::GetTickCount64();
    for (int i = 0; i < g_pendingReleaseCount;) {
        if (now >= g_pendingReleases[i].tick) {
            PostKeyboardKey(g_pendingReleases[i].vk, false);
            g_pendingReleases[i] =
                g_pendingReleases[g_pendingReleaseCount - 1];
            --g_pendingReleaseCount;
        } else {
            ++i;
        }
    }
}

void TapKeyboardKey(int vk) {
    PostKeyboardKey(vk, true);
    if (g_pendingReleaseCount <
        static_cast<int>(sizeof(g_pendingReleases) /
                         sizeof(g_pendingReleases[0]))) {
        g_pendingReleases[g_pendingReleaseCount++] = {
            vk, ::GetTickCount64() + 100};
    } else {
        PostKeyboardKey(vk, false);
    }
}

void PostMouseButton(bool down) {
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    if (window == nullptr) {
        return;
    }
    RECT client{};
    if (!::GetClientRect(window, &client)) {
        return;
    }
    const LPARAM position =
        MAKELPARAM((client.right - client.left) / 2,
                   (client.bottom - client.top) / 2);
    ::PostMessageW(window, down ? WM_LBUTTONDOWN : WM_LBUTTONUP,
                   down ? MK_LBUTTON : 0, position);
}

void ApplyButtonActions(const ControllerInputSnapshot& state) {
    static ControllerInputSnapshot previous;
    static bool fireDown = false;
    static bool runDown = false;
    const bool gameplay = CameraReadbackGameplayActive();

    if (gameplay && state.interactDown && !previous.interactDown) {
        TapKeyboardKey('E');
    }
    if (gameplay && state.reloadDown && !previous.reloadDown) {
        TapKeyboardKey('R');
    }
    if (gameplay && state.weaponModeDown && !previous.weaponModeDown) {
        TapKeyboardKey('T');
    }
    if (gameplay && state.statusDown && !previous.statusDown) {
        TapKeyboardKey(VK_F2);
    }
    if (gameplay && state.sneakDown && !previous.sneakDown) {
        TapKeyboardKey(VK_SPACE);
    }
    if (gameplay && state.dropDown && !previous.dropDown) {
        TapKeyboardKey('G');
    }
    if (!gameplay && state.interactDown && !previous.interactDown) {
        TapKeyboardKey(VK_RETURN);
    }
    if (!gameplay && state.reloadDown && !previous.reloadDown) {
        TapKeyboardKey(VK_ESCAPE);
    }

    const bool wantFire = gameplay && state.fireDown;
    if (wantFire != fireDown) {
        PostMouseButton(wantFire);
        fireDown = wantFire;
    }
    const bool wantRun = gameplay && state.runDown;
    if (wantRun != runDown) {
        PostKeyboardKey(VK_LSHIFT, wantRun);
        runDown = wantRun;
    }
    previous = state;
    ProcessPendingKeyReleases();
}

SHORT WINAPI HookGetAsyncKeyState(int vk) {
    const SHORT native = g_real_get_async_key_state != nullptr
                             ? g_real_get_async_key_state(vk)
                             : 0;
    bool edge = false;
    if (!SyntheticDown(vk, &edge)) {
        return native;
    }
    if (g_synthetic_keys.fetch_add(1) == 0) {
        H47VR_LOG_INFO("controller input: synthesizing engine keyboard state "
                       "through GetAsyncKeyState");
    }
    return static_cast<SHORT>(0x8000 | (edge ? 1 : 0));
}

}  // namespace

void ControllerInputPublish(const ControllerInputSnapshot& state) {
    ControllerInputSnapshot next = state;
    next.leftX = ClampAxis(next.leftX);
    next.leftY = ClampAxis(next.leftY);
    next.rightX = ClampAxis(next.rightX);
    next.rightY = ClampAxis(next.rightY);
    next.leftActive = AxisDown(next.leftX) || AxisDown(next.leftY);
    next.rightActive = AxisDown(next.rightX) || AxisDown(next.rightY);

    std::lock_guard<std::mutex> lock(g_mutex);
    if (next.escapeDown && !g_physical.escapeDown) {
        next.escapeGeneration = g_physical.escapeGeneration + 1;
    } else {
        next.escapeGeneration = g_physical.escapeGeneration;
    }
    if (next.snapDown && !g_physical.snapDown) {
        next.snapGeneration = g_physical.snapGeneration + 1;
    } else {
        next.snapGeneration = g_physical.snapGeneration;
    }
    g_physical = next;
}

ControllerInputSnapshot ControllerInputGet() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_debug_active ? g_debug : g_physical;
}

bool ControllerInputStart() {
    g_test_path =
        JoinPath(mod::GameDirectory(), L"h47vr-controller-test.txt");
    H47VR_LOG_INFO("controller input: ready - OpenXR sticks map to movement "
                   "and native turning; menus map to arrow keys");
    ControllerInputRearm();
    return true;
}

void ControllerInputRearm() {
    if (g_installed) {
        return;
    }
    if (HookSymbol(L"user32.dll", "GetAsyncKeyState",
                   reinterpret_cast<void*>(&HookGetAsyncKeyState),
                   reinterpret_cast<void**>(&g_real_get_async_key_state))) {
        g_installed = true;
        H47VR_LOG_INFO("controller input: hooked user32 GetAsyncKeyState");
    }
}

void ControllerInputFrame(unsigned long frame) {
    RefreshDebugOverride();
    const ControllerInputSnapshot state = ControllerInputGet();
    ApplyDirectKeyState(state);
    ApplyWindowKeyState(state);
    ApplyButtonActions(state);

    static unsigned long long handledEscape = 0;
    static bool escapeDownPosted = false;
    static HWND escapeWindow = nullptr;
    static ULONGLONG escapeReleaseTick = 0;
    if (frame < 240) {
        // A/B is often held while the user starts a VR session. Consume that
        // initial state without turning it into an ESC that closes the intro.
        handledEscape = state.escapeGeneration;
    } else if (state.valid && state.escapeDown && state.escapeGeneration != 0 &&
        state.escapeGeneration != handledEscape) {
        handledEscape = state.escapeGeneration;
        escapeWindow = reinterpret_cast<HWND>(WindowKeepaliveWindow());
        if (escapeWindow != nullptr) {
            const LPARAM down = MAKELPARAM(1, 0x01);
            const bool postedDown =
                ::PostMessageW(escapeWindow, WM_KEYDOWN, VK_ESCAPE, down) != FALSE;
            H47VR_LOG_INFO("controller input: controller ESC key-down %s",
                           postedDown ? "posted" : "post failed");
            if (postedDown) {
                escapeDownPosted = true;
                escapeReleaseTick = ::GetTickCount64() + 120;
            }
        }
    }
    if (escapeDownPosted && ::GetTickCount64() >= escapeReleaseTick) {
        const LPARAM up = static_cast<LPARAM>(0xC0010001u);
        const bool postedUp =
            ::PostMessageW(escapeWindow, WM_KEYUP, VK_ESCAPE, up) != FALSE;
        H47VR_LOG_INFO("controller input: controller ESC key-up %s",
                       postedUp ? "posted" : "post failed");
        escapeDownPosted = false;
    }
}

}  // namespace h47vr
