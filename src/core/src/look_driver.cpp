#include "h47vr/look_driver.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"
#include "h47vr/window_keepalive.hpp"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <intrin.h>
#include <sstream>
#include <string>

namespace h47vr {
namespace {

std::atomic<unsigned long> g_turns{0};
std::atomic<unsigned long> g_messages{0};
std::atomic<unsigned long> g_post_failures{0};
std::atomic<unsigned long> g_warp_swallowed{0};
std::atomic<bool> g_cursor_guard{false};

// One turn in flight. The turn is split across frames the way a hand moves a
// mouse, because the engine's handler returns early when the delta is zero and
// treats a large single jump as noise.
int g_dx = 0;
int g_dy = 0;
int g_steps_total = 0;
int g_steps_done = 0;
int g_sent_x = 0;
int g_sent_y = 0;
int g_rate = 1;
int g_wait = 0;
unsigned long g_next_poll = 0;
bool g_reported_window = false;

// Dead reckoning of what we have asked the engine's camera to do. The engine
// integrates the deltas we post into the player's aim, so this stays true as
// long as nothing else turns the player - which is exactly the case once the
// headset owns the mouse. It is what lets a head pose be an absolute target
// instead of a stream of relative nudges.
double g_yaw_deg = 0.0;
double g_pitch_deg = 0.0;
double g_deg_per_px = 0.2;
double g_turn_deg_per_px = 0.2;

// Rounds to nearest instead of truncating, so a turn split over N frames lands
// on the requested pixel count exactly even when the split is not whole
// (`-40 / 15` truncates to -2 and loses six pixels over a turn).
int Scale(int total, int numerator, int denominator) {
    const double value = static_cast<double>(total) * numerator / denominator;
    return static_cast<int>(value < 0 ? value - 0.5 : value + 0.5);
}

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

struct Request {
    int dx = 0;
    int dy = 0;
    int steps = 15;
    int rate = 1;
    bool has_yaw = false;
    bool has_pitch = false;
    double yaw = 0.0;
    double pitch = 0.0;
    double deg_per_px = 0.0;
};

bool ParseRequest(const std::string& line, Request& request) {
    std::istringstream stream(line);
    std::string token;
    bool any = false;
    while (stream >> token) {
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = token.substr(0, equals);
        const std::string text = token.substr(equals + 1);
        const int value = std::atoi(text.c_str());
        if (key == "dx") {
            request.dx = value;
            any = true;
        } else if (key == "dy") {
            request.dy = value;
            any = true;
        } else if (key == "yaw") {
            request.yaw = std::atof(text.c_str());
            request.has_yaw = true;
            any = true;
        } else if (key == "pitch") {
            request.pitch = std::atof(text.c_str());
            request.has_pitch = true;
            any = true;
        } else if (key == "degperpx") {
            request.deg_per_px = std::atof(text.c_str());
        } else if (key == "steps") {
            request.steps = value < 1 ? 1 : value;
        } else if (key == "rate") {
            request.rate = value < 1 ? 1 : value;
        }
    }
    if (request.has_yaw || request.has_pitch) {
        return true;
    }
    return any && (request.dx != 0 || request.dy != 0);
}

// The current dead-reckoned aim, for the tooling to read back.
void WriteState() {
    const std::wstring path = JoinPath(mod::GameDirectory(), L"h47vr-lookstate.txt");
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        return;
    }
    file << "yaw=" << g_yaw_deg << " pitch=" << g_pitch_deg
         << " degperpx=" << g_deg_per_px << "\n";
}

// The engine's mouse handler re-centres the cursor with SetCursorPos after every
// move. That is harmless for a real mouse (the cursor is already captured), but
// when the mod posts the move it yanks the user's pointer into the game window
// and onto whatever is underneath it. Swallowing just that one call keeps the
// desktop usable and changes nothing the engine sees: it has already stored the
// new position and the deltas before it warps.
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
SetCursorPosFn g_real_set_cursor_pos = nullptr;

bool CalledFromMouseHandler(void* address) {
    const HMODULE renderer = LoadedModule(L"RenderOpenGL.dll");
    if (renderer == nullptr || address == nullptr) {
        return false;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(renderer);
    const auto caller = reinterpret_cast<std::uintptr_t>(address);
    // The mouse handler is the only code in this range; the other SetCursorPos
    // call in the renderer is the one-off window setup at 0x2B8D0.
    return caller >= base + 0x29AC0 && caller < base + 0x29E50;
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (CalledFromMouseHandler(_ReturnAddress())) {
        ++g_warp_swallowed;
        return TRUE;
    }
    return g_real_set_cursor_pos(x, y);
}

// Posts one mouse-move at the client-area centre plus the step. The engine
// subtracts that centre itself, so the message carries an absolute position
// whose offset from the centre is the delta it should see.
void PostStep(int stepX, int stepY) {
    HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    if (window == nullptr) {
        ++g_post_failures;
        return;
    }

    RECT client{};
    if (!::GetClientRect(window, &client)) {
        ++g_post_failures;
        return;
    }
    const int centreX = (client.right - client.left) / 2;
    const int centreY = (client.bottom - client.top) / 2;
    const int x = centreX + stepX;
    const int y = centreY + stepY;

    if (::PostMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y))) {
        ++g_messages;
    } else {
        ++g_post_failures;
    }
}

void PollRequest(unsigned long frame) {
    const std::wstring path = JoinPath(mod::GameDirectory(), L"h47vr-look.txt");
    if (!FileExists(path)) {
        return;
    }

    const std::string line = ReadFirstLine(path);
    // Consumed on sight, exactly like h47vr-go.txt: one write is one turn.
    if (!::DeleteFileW(path.c_str())) {
        H47VR_LOG_WARN("look driver: could not remove %s (error %lu)",
                       Narrow(path).c_str(), ::GetLastError());
    }

    Request request;
    if (!ParseRequest(line, request)) {
        H47VR_LOG_WARN("look driver: cannot read a turn from '%s' "
                       "(expected 'dx=<px> dy=<px> [steps=<n>] [rate=<frames>]')",
                       line.c_str());
        return;
    }

    g_dx = request.dx;
    g_dy = request.dy;
    g_turn_deg_per_px = request.deg_per_px > 0.0 ? request.deg_per_px : g_deg_per_px;

    // An absolute request is turned into the pixel delta that reaches it. The
    // engine has no absolute look-at entry point we can call, but it does
    // integrate deltas - so the target minus our own estimate is the delta.
    if (request.has_yaw || request.has_pitch) {
        const double targetYaw = request.has_yaw ? request.yaw : g_yaw_deg;
        const double targetPitch = request.has_pitch ? request.pitch : g_pitch_deg;
        g_dx = static_cast<int>((targetYaw - g_yaw_deg) / g_turn_deg_per_px);
        g_dy = static_cast<int>((targetPitch - g_pitch_deg) / g_turn_deg_per_px);
        H47VR_LOG_INFO("look driver: target yaw=%.2f pitch=%.2f deg (from yaw=%.2f "
                       "pitch=%.2f, %.4f deg/px) -> dx=%d dy=%d",
                       targetYaw, targetPitch, g_yaw_deg, g_pitch_deg,
                       g_turn_deg_per_px, g_dx, g_dy);
        if (g_dx == 0 && g_dy == 0) {
            H47VR_LOG_INFO("look driver: already at the requested aim");
            return;
        }
    }

    g_steps_total = request.steps;
    g_steps_done = 0;
    g_sent_x = 0;
    g_sent_y = 0;
    g_rate = request.rate;
    g_wait = 0;

    // Recorded because it is the whole point of this route: the messages are
    // posted into the engine's own queue, so the turn works whether or not the
    // game owns the foreground. `tools/turn.ps1` needs the foreground; this
    // does not.
    const HWND window = reinterpret_cast<HWND>(WindowKeepaliveWindow());
    const HWND foreground = ::GetForegroundWindow();
    H47VR_LOG_INFO("look driver: turn dx=%d dy=%d over %d frame(s) (rate %d, frame %lu)",
                   g_dx, g_dy, g_steps_total, g_rate, frame);
    H47VR_LOG_INFO("look driver: window %p, foreground window %p - %s", window, foreground,
                   window != nullptr && window == foreground
                       ? "the game is focused"
                       : "the game is in the background");
}

}  // namespace

bool LookDriverStart() {
    LookDriverRearm();
    WriteState();
    H47VR_LOG_INFO("look driver: ready - write %s with 'dx=.. dy=..' to turn the "
                   "engine's own camera (no focus, no synthetic input)",
                   Narrow(JoinPath(mod::GameDirectory(), L"h47vr-look.txt")).c_str());
    H47VR_LOG_INFO("look driver: absolute aiming available - 'yaw=<deg> pitch=<deg> "
                   "[degperpx=<deg>]' targets a pose (%.4f deg/px by default, %s)",
                   g_deg_per_px,
                   Narrow(JoinPath(mod::GameDirectory(), L"h47vr-lookstate.txt")).c_str());
    return true;
}

void LookDriverRearm() {
    if (g_cursor_guard.load()) {
        return;
    }
    if (LoadedModule(L"RenderOpenGL.dll") == nullptr) {
        return;
    }
    if (HookSymbol(L"user32.dll", "SetCursorPos",
                   reinterpret_cast<void*>(&HookSetCursorPos),
                   reinterpret_cast<void**>(&g_real_set_cursor_pos))) {
        g_cursor_guard = true;
        H47VR_LOG_INFO("look driver: cursor guard installed - the engine's cursor "
                       "warp is swallowed so turns do not move the desktop pointer");
    }
}

void LookDriverFrame(unsigned long frame) {
    if (g_steps_done < g_steps_total) {
        if (g_wait > 0) {
            --g_wait;
            return;
        }

        const int next = g_steps_done + 1;
        const int wantX = Scale(g_dx, next, g_steps_total);
        const int wantY = Scale(g_dy, next, g_steps_total);
        PostStep(wantX - g_sent_x, wantY - g_sent_y);
        g_sent_x = wantX;
        g_sent_y = wantY;
        ++g_steps_done;
        g_wait = g_rate - 1;

        if (g_steps_done == g_steps_total) {
            g_yaw_deg += g_dx * g_turn_deg_per_px;
            g_pitch_deg += g_dy * g_turn_deg_per_px;
            g_deg_per_px = g_turn_deg_per_px;
            WriteState();
            ++g_turns;
            H47VR_LOG_INFO("look driver: turn complete (dx=%d dy=%d, %d message(s), "
                           "%lu posted so far, %lu failed, now yaw=%.2f pitch=%.2f, frame %lu)",
                           g_dx, g_dy, g_steps_total, g_messages.load(),
                           g_post_failures.load(), g_yaw_deg, g_pitch_deg, frame);
        }
        return;
    }

    // The file is only checked a few times a second; there is no need to stat it
    // sixty times a second either.
    if (frame < g_next_poll) {
        return;
    }
    g_next_poll = frame + 15;
    PollRequest(frame);
}

void LookDriverReport(unsigned long frame) {
    if (!g_reported_window) {
        g_reported_window = true;
        H47VR_LOG_INFO("look driver: game window %p", WindowKeepaliveWindow());
    }
    H47VR_LOG_INFO("look driver: %lu turn(s), %lu mouse message(s) posted, %lu failed, "
                   "%lu cursor warp(s) swallowed (frame %lu)",
                   g_turns.load(), g_messages.load(), g_post_failures.load(),
                   g_warp_swallowed.load(), frame);
}

}  // namespace h47vr
