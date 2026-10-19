#include "h47vr/look_driver.hpp"

#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/window_keepalive.hpp"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace h47vr {
namespace {

std::atomic<unsigned long> g_turns{0};
std::atomic<unsigned long> g_messages{0};
std::atomic<unsigned long> g_post_failures{0};

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
        const int value = std::atoi(token.substr(equals + 1).c_str());
        if (key == "dx") {
            request.dx = value;
            any = true;
        } else if (key == "dy") {
            request.dy = value;
            any = true;
        } else if (key == "steps") {
            request.steps = value < 1 ? 1 : value;
        } else if (key == "rate") {
            request.rate = value < 1 ? 1 : value;
        }
    }
    return any && (request.dx != 0 || request.dy != 0);
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
    H47VR_LOG_INFO("look driver: ready - write %s with 'dx=.. dy=..' to turn the "
                   "engine's own camera (no focus, no synthetic input)",
                   Narrow(JoinPath(mod::GameDirectory(), L"h47vr-look.txt")).c_str());
    return true;
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
            ++g_turns;
            H47VR_LOG_INFO("look driver: turn complete (dx=%d dy=%d, %d message(s), "
                           "%lu posted so far, %lu failed, frame %lu)",
                           g_dx, g_dy, g_steps_total, g_messages.load(),
                           g_post_failures.load(), frame);
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
    H47VR_LOG_INFO("look driver: %lu turn(s), %lu mouse message(s) posted, %lu failed "
                   "(frame %lu)",
                   g_turns.load(), g_messages.load(), g_post_failures.load(), frame);
}

}  // namespace h47vr
