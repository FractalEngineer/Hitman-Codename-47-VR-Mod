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
constexpr float kMenuPointerSpeedPixelsPerSecond = 720.0f;
constexpr float kMenuPointerDeadzone = 0.12f;

std::mutex g_mutex;
ControllerInputSnapshot g_physical;
ControllerInputSnapshot g_debug;
std::atomic<unsigned long long> g_escape_generation_seen{0};
bool g_debug_active = false;
bool g_installed = false;
std::atomic<unsigned long> g_synthetic_keys{0};
std::wstring g_test_path;

bool g_menu_pointer_initialized = false;
bool g_menu_pointer_reported = false;
float g_menu_pointer_x = 0.0f;
float g_menu_pointer_y = 0.0f;
ULONGLONG g_menu_pointer_tick = 0;
bool g_menu_click_held = false;
ULONGLONG g_menu_click_release_tick = 0;
std::atomic<unsigned long> g_menu_moves{0};
std::atomic<unsigned long> g_menu_clicks{0};

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
        float interact = 0.0f;
        float reload = 0.0f;
        float weapon = 0.0f;
        float status = 0.0f;
        float run = 0.0f;
        float fire = 0.0f;
        float sneak = 0.0f;
        float drop = 0.0f;
        ReadNumber(line, "esc", &escape);
        ReadNumber(line, "snap", &snap);
        ReadNumber(line, "interact", &interact);
        ReadNumber(line, "reload", &reload);
        ReadNumber(line, "weapon", &weapon);
        ReadNumber(line, "status", &status);
        ReadNumber(line, "run", &run);
        ReadNumber(line, "fire", &fire);
        ReadNumber(line, "sneak", &sneak);
        ReadNumber(line, "drop", &drop);
        next.escapeDown = escape != 0.0f;
        next.snapDown = snap != 0.0f;
        next.interactDown = interact != 0.0f;
        next.reloadDown = reload != 0.0f;
        next.weaponModeDown = weapon != 0.0f;
        next.statusDown = status != 0.0f;
        next.runDown = run != 0.0f;
        next.fireDown = fire != 0.0f;
        next.sneakDown = sneak != 0.0f;
        next.dropDown = drop != 0.0f;
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
    } else {
        x = state.leftX;
        y = state.leftY;
    }

    int count = 0;
    if (gameplay) {
        if (y < -kStickActivate) keys[count++] = 'S';
        if (y > kStickActivate) keys[count++] = 'W';
        if (x < -kStickActivate) keys[count++] = 'A';
        if (x > kStickActivate) keys[count++] = 'D';
    } else {
        if (y < -kStickActivate) keys[count++] = VK_DOWN;
        if (y > kStickActivate) keys[count++] = VK_UP;
        if (x < -kStickActivate) keys[count++] = VK_LEFT;
        if (x > kStickActivate) keys[count++] = VK_RIGHT;
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

bool EngineRenderSize(int* width, int* height) {
    void* slot = ResolveSymbol(
        L"Globals.dll", "?g_pSysInterface@@3PAVZSysInterface@@A");
    if (slot == nullptr) {
        return false;
    }
    auto* sysInterface = *reinterpret_cast<unsigned char**>(slot);
    if (sysInterface == nullptr) {
        return false;
    }
    const int renderWidth =
        *reinterpret_cast<int*>(sysInterface + 0x19);
    const int renderHeight =
        *reinterpret_cast<int*>(sysInterface + 0x1D);
    if (renderWidth <= 0 || renderHeight <= 0 ||
        renderWidth > 0x3FFF || renderHeight > 0x3FFF) {
        return false;
    }
    if (width != nullptr) {
        *width = renderWidth;
    }
    if (height != nullptr) {
        *height = renderHeight;
    }
    return true;
}

bool ClientPointFromRender(float renderX, float renderY, LPARAM* point) {
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    if (window == nullptr) {
        return false;
    }
    RECT client{};
    if (!::GetClientRect(window, &client) ||
        client.right <= client.left || client.bottom <= client.top) {
        return false;
    }
    const int clientWidth = static_cast<int>(client.right - client.left);
    const int clientHeight = static_cast<int>(client.bottom - client.top);
    int renderWidth = clientWidth;
    int renderHeight = clientHeight;
    EngineRenderSize(&renderWidth, &renderHeight);
    const int x = std::clamp(
        static_cast<int>(renderX * clientWidth /
                         (std::max)(renderWidth, 1)),
        0, clientWidth - 1);
    const int y = std::clamp(
        static_cast<int>(renderY * clientHeight /
                         (std::max)(renderHeight, 1)),
        0, clientHeight - 1);
    *point = MAKELPARAM(x, y);
    return true;
}

void PostMenuPointerMove() {
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    LPARAM position = 0;
    if (window == nullptr ||
        !ClientPointFromRender(g_menu_pointer_x, g_menu_pointer_y, &position)) {
        return;
    }
    if (::PostMessageW(window, WM_MOUSEMOVE, 0, position)) {
        ++g_menu_moves;
    }
}

void PostMenuPointerClick() {
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    LPARAM position = 0;
    if (window == nullptr ||
        !ClientPointFromRender(g_menu_pointer_x, g_menu_pointer_y, &position)) {
        return;
    }
    if (::PostMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, position)) {
        g_menu_click_held = true;
        g_menu_click_release_tick = ::GetTickCount64() + 80;
        H47VR_LOG_INFO("controller input: menu pointer click #%lu at %.0f,%.0f",
                       ++g_menu_clicks, g_menu_pointer_x, g_menu_pointer_y);
    }
}

void ProcessMenuPointer(const ControllerInputSnapshot& state, bool gameplay) {
    if (gameplay) {
        g_menu_pointer_initialized = false;
        g_menu_pointer_reported = false;
        g_menu_pointer_tick = 0;
        return;
    }

    int width = 0;
    int height = 0;
    if (!EngineRenderSize(&width, &height)) {
        HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
        RECT client{};
        if (window == nullptr || !::GetClientRect(window, &client)) {
            return;
        }
        width = client.right - client.left;
        height = client.bottom - client.top;
    }
    if (width <= 0 || height <= 0) {
        return;
    }

    const ULONGLONG now = ::GetTickCount64();
    if (!g_menu_pointer_initialized) {
        const bool requested =
            std::fabs(state.rightX) >= kMenuPointerDeadzone ||
            std::fabs(state.rightY) >= kMenuPointerDeadzone ||
            state.interactDown || state.fireDown;
        if (!requested) {
            return;
        }
        g_menu_pointer_initialized = true;
        g_menu_pointer_x = width * 0.5f;
        g_menu_pointer_y = height * 0.5f;
        g_menu_pointer_tick = now;
        PostMenuPointerMove();
        return;
    }

    const float dt = g_menu_pointer_tick == 0
                         ? 0.0f
                         : std::clamp((now - g_menu_pointer_tick) / 1000.0f,
                                      0.0f, 0.05f);
    g_menu_pointer_tick = now;
    const float x = std::fabs(state.rightX) >= kMenuPointerDeadzone
                        ? state.rightX
                        : 0.0f;
    const float y = std::fabs(state.rightY) >= kMenuPointerDeadzone
                        ? -state.rightY
                        : 0.0f;
    if (x == 0.0f && y == 0.0f) {
        return;
    }

    g_menu_pointer_x = std::clamp(
        g_menu_pointer_x + x * kMenuPointerSpeedPixelsPerSecond * dt,
        1.0f, static_cast<float>(width - 2));
    g_menu_pointer_y = std::clamp(
        g_menu_pointer_y + y * kMenuPointerSpeedPixelsPerSecond * dt,
        1.0f, static_cast<float>(height - 2));
    PostMenuPointerMove();
    if (!g_menu_pointer_reported) {
        g_menu_pointer_reported = true;
        H47VR_LOG_INFO("controller input: right stick is driving the native "
                       "menu pointer (%dx%d, %lu move(s))", width, height,
                       g_menu_moves.load());
    }
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
    const bool menuClickDown =
        !gameplay && (state.interactDown || state.fireDown);
    const bool previousMenuClickDown =
        !gameplay && (previous.interactDown || previous.fireDown);
    if (menuClickDown && !previousMenuClickDown &&
        g_menu_pointer_initialized) {
        PostMenuPointerClick();
    } else if (!gameplay && state.interactDown && !previous.interactDown &&
               !g_menu_pointer_initialized) {
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
    if (g_menu_click_held && ::GetTickCount64() >= g_menu_click_release_tick) {
        HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
        LPARAM position = 0;
        if (window != nullptr &&
            ClientPointFromRender(g_menu_pointer_x, g_menu_pointer_y,
                                  &position)) {
            ::PostMessageW(window, WM_LBUTTONUP, 0, position);
        }
        g_menu_click_held = false;
    }
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
    ProcessMenuPointer(state, CameraReadbackGameplayActive());
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
