#include "h47vr/look_driver.hpp"

#include "h47vr/camera_readback.hpp"
#include "h47vr/controller_input.hpp"
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
#include <intrin.h>
#include <sstream>
#include <string>

namespace h47vr {
namespace {

std::atomic<unsigned long> g_turns{0};
std::atomic<unsigned long> g_messages{0};
std::atomic<unsigned long> g_post_failures{0};
std::atomic<unsigned long> g_warp_swallowed{0};
std::atomic<unsigned long> g_head_posts{0};
std::atomic<unsigned long> g_native_posts{0};
std::atomic<bool> g_cursor_guard{false};
thread_local bool g_synthetic_mouse_active = false;
std::atomic<double> g_scene_pitch_degrees{0.0};
std::atomic<bool> g_native_path_ready{false};

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
unsigned long g_finish_frame = 0;
unsigned long g_finish_deadline = 0;
bool g_refresh_after_readback = false;
bool g_pending_absolute = false;
bool g_pending_has_yaw = false;
bool g_pending_has_pitch = false;
double g_pending_yaw = 0.0;
double g_pending_pitch = 0.0;
double g_pending_deg_per_px = 0.0;
int g_pending_steps = 15;
int g_pending_rate = 1;
bool g_reported_window = false;

// The predicted head pose is an absolute target in the runtime's local space.
// The first valid sample becomes zero so tracking starts aligned with the game
// camera rather than whichever direction the headset happened to face at boot.
bool g_head_valid = false;
bool g_head_reference_valid = false;
double g_head_yaw = 0.0;
double g_head_pitch = 0.0;
double g_head_reference_yaw = 0.0;
double g_head_reference_pitch = 0.0;

// A headset can legitimately move much farther in one frame than a mouse. Keep
// each posted message within the engine's comfortable range, then let the next
// frames close the remaining error. The deadband avoids a stream of one-pixel
// corrections from prediction noise.
constexpr double kHeadDeadbandDegrees = 0.08;
constexpr double kNativeCorrectionDeadbandDegrees = 0.01;
constexpr int kHeadMaxPixelsPerFrame = 192;
constexpr double kNativeMaxYawStepDegrees = 16.0;
constexpr double kControllerTurnDegreesPerSecond = 180.0;
constexpr double kSnapTurnDegrees = 30.0;

// Dead reckoning of what we have asked the engine's camera to do. The engine
// integrates the deltas we post into the player's aim, so this stays true as
// long as nothing else turns the player - which is exactly the case once the
// headset owns the mouse. It is what lets a head pose be an absolute target
// instead of a stream of relative nudges.
// The engine integrates the deltas itself, so the pixel total *is* the aim state.
// Degrees are derived from it, which means recalibrating does not invalidate a
// recentre: yaw=0 still posts exactly the pixels needed to undo the total.
double g_px_x = 0.0;
double g_px_y = 0.0;
// Measured from the engine's live MouseConCam orientation on 2026-10-19:
// 100 px produced 12.8915 degrees and 450 px produced 58.0120, both 0.128916
// deg/px. The earlier 0.09 estimate came from looking at the tiled floor.
constexpr double kReadbackDegPerPixel = 0.1289156;
double g_deg_per_px = kReadbackDegPerPixel;
double g_turn_deg_per_px = kReadbackDegPerPixel;
unsigned long long g_last_snap_generation = 0;
ULONGLONG g_last_controller_turn_tick = 0;

std::wstring g_debug_head_path;
std::wstring g_native_pitch_path;
bool g_debug_head_active = false;
double g_last_debug_head_yaw = 0.0;
double g_last_debug_head_pitch = 0.0;
std::atomic<bool> g_native_pitch_enabled{false};
std::atomic<unsigned long> g_native_pitch_posts{0};

// Absolute aim stays relative to the first MouseConCam pose seen for the
// current camera object. That preserves `yaw=0` as a recentre even though the
// engine's own yaw has an arbitrary world-space zero.
bool g_reference_valid = false;
void* g_reference_object = nullptr;
unsigned long g_reference_epoch = 0;
double g_reference_yaw = 0.0;
double g_reference_pitch = 0.0;

double Wrap180(double degrees) {
    while (degrees > 180.0) {
        degrees -= 360.0;
    }
    while (degrees <= -180.0) {
        degrees += 360.0;
    }
    return degrees;
}

// Yaw is positive to the right. Pitch is positive down, matching the `dy`
// convention and tools/look.ps1; the engine stores its elevation positive up.
bool ReadCurrentAim(double* yaw, double* pitch, CameraReadback* readback) {
    if (!CameraReadbackGameplayActive()) {
        return false;
    }
    CameraReadback state;
    if (!CameraReadbackGet(&state)) {
        return false;
    }
    if (!g_reference_valid || state.object != g_reference_object ||
        state.epoch != g_reference_epoch) {
        if (g_reference_valid) {
            g_px_x = 0.0;
            g_px_y = 0.0;
        }
        g_reference_valid = true;
        g_reference_object = state.object;
        g_reference_epoch = state.epoch;
        g_reference_yaw = state.yaw;
        g_reference_pitch = -state.pitch;
        H47VR_LOG_INFO("look driver: camera readback reference at object %p: "
                       "engine yaw=%.3f elevation=%.3f, relative yaw=%.3f pitch=%.3f",
                       state.object, state.yaw, state.pitch,
                       Wrap180(state.yaw - g_reference_yaw),
                       -state.pitch - g_reference_pitch);
    }
    if (yaw != nullptr) {
        *yaw = Wrap180(state.yaw - g_reference_yaw);
    }
    if (pitch != nullptr) {
        *pitch = -state.pitch - g_reference_pitch;
    }
    if (readback != nullptr) {
        *readback = state;
    }
    return true;
}

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
    CameraReadback readback;
    double yaw = g_px_x * g_deg_per_px;
    double pitch = g_px_y * g_deg_per_px;
    const bool measured = ReadCurrentAim(&yaw, &pitch, &readback);
    file << "yaw=" << yaw << " pitch=" << pitch
         << " degperpx=" << g_deg_per_px << " px=" << g_px_x << "," << g_px_y
         << " source=" << (measured ? "readback" : "estimate");
    if (measured) {
        file << " rawyaw=" << readback.yaw << " rawelevation=" << readback.pitch
             << " camera=" << readback.object << " epoch=" << readback.epoch;
    }
    if (g_head_valid && g_head_reference_valid) {
        file << " headtarget="
             << Wrap180(g_head_yaw - g_head_reference_yaw)
             << " headpitch="
             << -(g_head_pitch - g_head_reference_pitch)
             << " headerror=" << Wrap180(
                    Wrap180(g_head_yaw - g_head_reference_yaw) - yaw);
    }
    file << "\n";
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
    if (CalledFromMouseHandler(_ReturnAddress()) &&
        (g_synthetic_mouse_active || LookDriverHeadTrackingActive())) {
        ++g_warp_swallowed;
        return TRUE;
    }
    return g_real_set_cursor_pos(x, y);
}

// Posts one mouse-move at the client-area centre plus the step. The engine
// subtracts that centre itself, so the message carries an absolute position
// whose offset from the centre is the delta it should see.
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

bool g_input_geometry_reported = false;

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
    int centreX = (client.right - client.left) / 2;
    int centreY = (client.bottom - client.top) / 2;
    int renderWidth = 0;
    int renderHeight = 0;
    if (EngineRenderSize(&renderWidth, &renderHeight)) {
        centreX = renderWidth / 2;
        centreY = renderHeight / 2;
    }
    if (!g_input_geometry_reported) {
        g_input_geometry_reported = true;
        H47VR_LOG_INFO("look driver: mouse coordinates client=%dx%d "
                       "render=%dx%d, centre=%d,%d",
                       client.right - client.left, client.bottom - client.top,
                       renderWidth, renderHeight, centreX, centreY);
    }
    const int x = centreX + stepX;
    const int y = centreY + stepY;

    LookDriverBeginSyntheticMouse();
    ::SendMessageW(window, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    LookDriverEndSyntheticMouse();
    ++g_messages;
}

void BeginTurn(unsigned long frame, int steps, int rate) {
    g_steps_total = steps;
    g_steps_done = 0;
    g_sent_x = 0;
    g_sent_y = 0;
    g_rate = rate;
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

    // An absolute request is turned into the pixel delta that reaches it. It
    // must wait for MouseConCam: before that camera class is active, the engine
    // can consume the posted messages without turning anything.
    if (request.has_yaw || request.has_pitch) {
        CameraReadback readback;
        double currentYaw = g_px_x * g_turn_deg_per_px;
        double currentPitch = g_px_y * g_turn_deg_per_px;
        const bool measured = ReadCurrentAim(&currentYaw, &currentPitch, &readback);
        if (!measured) {
            g_pending_absolute = true;
            g_pending_has_yaw = request.has_yaw;
            g_pending_has_pitch = request.has_pitch;
            g_pending_yaw = request.yaw;
            g_pending_pitch = request.pitch;
            g_pending_deg_per_px = request.deg_per_px;
            g_pending_steps = request.steps;
            g_pending_rate = request.rate;
            H47VR_LOG_INFO("look driver: absolute aim queued until MouseConCam "
                           "produces its first readback (yaw=%s%.2f pitch=%s%.2f)",
                           request.has_yaw ? "" : "hold ", request.yaw,
                           request.has_pitch ? "" : "hold ", request.pitch);
            return;
        }
        g_refresh_after_readback = false;
        const double targetYaw = request.has_yaw ? request.yaw : currentYaw;
        const double targetPitch = request.has_pitch ? request.pitch : currentPitch;
        const double deltaDegreesYaw = Wrap180(targetYaw - currentYaw);
        const double deltaDegreesPitch = targetPitch - currentPitch;
        const double deltaX = deltaDegreesYaw / g_turn_deg_per_px;
        const double deltaY = deltaDegreesPitch / g_turn_deg_per_px;
        g_dx = static_cast<int>(deltaX < 0 ? deltaX - 0.5 : deltaX + 0.5);
        g_dy = static_cast<int>(deltaY < 0 ? deltaY - 0.5 : deltaY + 0.5);
        H47VR_LOG_INFO("look driver: target yaw=%.2f pitch=%.2f deg (from yaw=%.2f "
                       "pitch=%.2f, %.4f deg/px, %s) -> dx=%d dy=%d",
                       targetYaw, targetPitch, currentYaw, currentPitch,
                       g_turn_deg_per_px,
                       measured ? "readback" : "estimate", g_dx, g_dy);
        if (g_dx == 0 && g_dy == 0) {
            H47VR_LOG_INFO("look driver: already at the requested aim");
            WriteState();
            return;
        }
    }

    BeginTurn(frame, request.steps, request.rate);
}

void TryPendingAbsolute(unsigned long frame) {
    if (!g_pending_absolute) {
        return;
    }

    CameraReadback readback;
    double currentYaw = 0.0;
    double currentPitch = 0.0;
    if (!ReadCurrentAim(&currentYaw, &currentPitch, &readback)) {
        return;
    }

    g_turn_deg_per_px =
        g_pending_deg_per_px > 0.0 ? g_pending_deg_per_px : g_deg_per_px;
    const double targetYaw = g_pending_has_yaw ? g_pending_yaw : currentYaw;
    const double targetPitch = g_pending_has_pitch ? g_pending_pitch : currentPitch;
    const double deltaX = Wrap180(targetYaw - currentYaw) / g_turn_deg_per_px;
    const double deltaY = (targetPitch - currentPitch) / g_turn_deg_per_px;
    g_dx = static_cast<int>(deltaX < 0 ? deltaX - 0.5 : deltaX + 0.5);
    g_dy = static_cast<int>(deltaY < 0 ? deltaY - 0.5 : deltaY + 0.5);

    const int steps = g_pending_steps;
    const int rate = g_pending_rate;
    g_pending_absolute = false;
    H47VR_LOG_INFO("look driver: MouseConCam readback active; resolving queued target "
                   "yaw=%.2f pitch=%.2f from yaw=%.2f pitch=%.2f -> dx=%d dy=%d",
                   targetYaw, targetPitch, currentYaw, currentPitch, g_dx, g_dy);
    if (g_dx == 0 && g_dy == 0) {
        H47VR_LOG_INFO("look driver: already at the queued aim");
        WriteState();
        return;
    }
    BeginTurn(frame, steps, rate);
}

void ApplyHeadPose(unsigned long frame) {
    if (!g_head_valid || !g_head_reference_valid ||
        g_steps_done < g_steps_total) {
        return;
    }

    CameraReadback readback;
    double currentYaw = 0.0;
    double currentPitch = 0.0;
    if (!ReadCurrentAim(&currentYaw, &currentPitch, &readback)) {
        // Menus, cutscenes and an unloaded front end have no MouseConCam
        // orientation to drive. That leaves their own camera alone.
        return;
    }

    const double targetYaw = Wrap180(g_head_yaw - g_head_reference_yaw);
    const double targetPitch = -(g_head_pitch - g_head_reference_pitch);
    const double errorYaw = Wrap180(targetYaw - currentYaw);
    const double errorPitch = targetPitch - currentPitch;
    // Pitch is rendered directly at the GL seam. Driving it through the
    // MouseConCam input path moves the follow-camera pivot vertically and
    // caused a slow sink through the floor.
    g_scene_pitch_degrees =
        g_native_pitch_enabled.load() ? 0.0f : static_cast<float>(errorPitch);
    if (g_native_path_ready.load()) {
        // The yaw correction was already handed to MouseConCam before the
        // engine updated its camera. Keep the legacy posting path only for a
        // readback state that cannot reach the native hook.
        return;
    }
    if (std::fabs(errorYaw) < kHeadDeadbandDegrees) {
        return;
    }

    double pixelsX = errorYaw / g_deg_per_px;
    pixelsX = std::clamp(pixelsX, -static_cast<double>(kHeadMaxPixelsPerFrame),
                         static_cast<double>(kHeadMaxPixelsPerFrame));
    const int dx = static_cast<int>(pixelsX < 0.0 ? pixelsX - 0.5 : pixelsX + 0.5);
    if (dx == 0) {
        return;
    }

    PostStep(dx, 0);
    g_px_x += dx;
    ++g_head_posts;
    if (g_head_posts.load() == 1) {
        H47VR_LOG_INFO("look driver: head tracking active; first correction "
                       "dx=%d (target yaw=%.2f, scene pitch=%.2f, frame %lu)",
                       dx, targetYaw, g_scene_pitch_degrees.load(), frame);
    }
}

bool ReadHeadPoseTest(double* yaw, double* pitch) {
    if (g_debug_head_path.empty() ||
        ::GetFileAttributesW(g_debug_head_path.c_str()) ==
            INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    const std::string line = ReadFirstLine(g_debug_head_path);
    if (line.empty()) {
        return false;
    }
    const std::size_t yawAt = line.find("yaw=");
    const std::size_t pitchAt = line.find("pitch=");
    if (yawAt == std::string::npos && pitchAt == std::string::npos) {
        return false;
    }
    *yaw = yawAt != std::string::npos
               ? std::atof(line.c_str() + yawAt + 4)
               : 0.0;
    *pitch = pitchAt != std::string::npos
                 ? std::atof(line.c_str() + pitchAt + 6)
                 : 0.0;
    return true;
}

}  // namespace

bool LookDriverStart() {
    g_debug_head_path =
        JoinPath(mod::GameDirectory(), L"h47vr-headpose.txt");
    g_native_pitch_path =
        JoinPath(mod::GameDirectory(), L"h47vr-nativepitch.txt");
    LookDriverRearm();
    WriteState();
    H47VR_LOG_INFO("look driver: ready - write %s with 'dx=.. dy=..' to turn the "
                   "engine's own camera (no focus, no synthetic input)",
                   Narrow(JoinPath(mod::GameDirectory(), L"h47vr-look.txt")).c_str());
    H47VR_LOG_INFO("look driver: absolute aiming available - 'yaw=<deg> pitch=<deg> "
                   "[degperpx=<deg>]' targets a pose (%.7f deg/px from camera "
                   "readback by default, %s)",
                   g_deg_per_px,
                   Narrow(JoinPath(mod::GameDirectory(), L"h47vr-lookstate.txt")).c_str());
    return true;
}

void LookDriverApplyNativeYaw() {
    if (g_steps_done < g_steps_total) {
        return;
    }

    CameraReadback readback;
    double currentYaw = 0.0;
    double currentPitch = 0.0;
    if (!ReadCurrentAim(&currentYaw, &currentPitch, &readback)) {
        return;
    }

    const ULONGLONG now = ::GetTickCount64();
    const double controllerDt =
        g_last_controller_turn_tick == 0
            ? 0.0
            : std::clamp((now - g_last_controller_turn_tick) / 1000.0, 0.0,
                         0.05);
    g_last_controller_turn_tick = now;

    double yawDelta = 0.0;
    if (g_head_valid && g_head_reference_valid) {
        const double targetYaw = Wrap180(g_head_yaw - g_head_reference_yaw);
        const double errorYaw = Wrap180(targetYaw - currentYaw);
        if (std::fabs(errorYaw) >= kNativeCorrectionDeadbandDegrees) {
            yawDelta += std::clamp(
                errorYaw, -kNativeMaxYawStepDegrees, kNativeMaxYawStepDegrees);
        }
    }

    const ControllerInputSnapshot controller = ControllerInputGet();
    double controllerTurnDegrees = 0.0;
    if (controller.valid) {
        if (std::fabs(controller.rightX) >= 0.08f) {
            controllerTurnDegrees += controller.rightX *
                                     kControllerTurnDegreesPerSecond *
                                         controllerDt;
        }
        if (controller.snapDown && controller.snapGeneration != 0 &&
            controller.snapGeneration != g_last_snap_generation) {
            g_last_snap_generation = controller.snapGeneration;
            if (controller.rightX < -0.25f) {
                controllerTurnDegrees -= kSnapTurnDegrees;
            } else if (controller.rightX > 0.25f) {
                controllerTurnDegrees += kSnapTurnDegrees;
            }
        }
    }
    // A controller turn rotates the player body, not the physical head. Move
    // the head reference with it or the absolute head target would immediately
    // pull the camera back to its pre-turn orientation.
    if (std::fabs(controllerTurnDegrees) > 0.0001) {
        yawDelta += controllerTurnDegrees;
        if (g_head_valid && g_head_reference_valid) {
            g_head_reference_yaw = Wrap180(g_head_reference_yaw -
                                           controllerTurnDegrees);
        }
    }

    const bool nativePitchActive =
        g_native_pitch_enabled.load() && g_head_valid &&
        g_head_reference_valid;
    if (std::fabs(yawDelta) < 0.001 && !nativePitchActive) {
        g_native_path_ready = true;
        return;
    }

    void* slot = ResolveSymbol(
        L"Globals.dll", "?g_pSysInterface@@3PAVZSysInterface@@A");
    if (slot == nullptr) {
        return;
    }
    auto* sysInterface = *reinterpret_cast<unsigned char**>(slot);
    if (sysInterface == nullptr) {
        return;
    }

    double pixels = yawDelta / kReadbackDegPerPixel;
    pixels = std::clamp(pixels, -static_cast<double>(kHeadMaxPixelsPerFrame),
                        static_cast<double>(kHeadMaxPixelsPerFrame));
    const int dx = static_cast<int>(pixels < 0.0 ? pixels - 0.5 : pixels + 0.5);
    if (dx == 0 && !nativePitchActive) {
        return;
    }

    if (dx != 0) {
        auto* accumulator = reinterpret_cast<float*>(sysInterface + 0xa95);
        *accumulator += static_cast<float>(dx);
    }
    g_native_path_ready = true;
    const unsigned long count = dx != 0 ? ++g_native_posts : g_native_posts.load();
    if (dx != 0 && (count == 1 || count % 300 == 0)) {
        H47VR_LOG_INFO("look driver: native MouseConCam yaw %+.3f deg "
                       "(%+d engine units), current yaw %.3f, camera %p, "
                       "native corrections %lu",
                       yawDelta, dx, currentYaw, readback.object, count);
    }

    if (nativePitchActive) {
        const double targetPitch =
            -(g_head_pitch - g_head_reference_pitch);
        const double errorPitch = targetPitch - currentPitch;
        if (std::fabs(errorPitch) >= kNativeCorrectionDeadbandDegrees) {
            const double pitchDelta = std::clamp(
                errorPitch, -kNativeMaxYawStepDegrees, kNativeMaxYawStepDegrees);
            double pitchPixels = pitchDelta / kReadbackDegPerPixel;
            pitchPixels = std::clamp(
                pitchPixels, -static_cast<double>(kHeadMaxPixelsPerFrame),
                static_cast<double>(kHeadMaxPixelsPerFrame));
            const int dy = static_cast<int>(
                pitchPixels < 0.0 ? pitchPixels - 0.5 : pitchPixels + 0.5);
            if (dy != 0) {
                auto* accumulator =
                    reinterpret_cast<float*>(sysInterface + 0xa99);
                *accumulator += static_cast<float>(dy);
                const unsigned long pitchCount = ++g_native_pitch_posts;
                if (pitchCount == 1 || pitchCount % 300 == 0) {
                    H47VR_LOG_INFO("look driver: native MouseConCam pitch "
                                   "%+.3f deg (%+d engine units), current "
                                   "pitch %.3f, corrections %lu",
                                   pitchDelta, dy, currentPitch, pitchCount);
                }
            }
        }
    }
}

void LookDriverBeginSyntheticMouse() {
    g_synthetic_mouse_active = true;
}

void LookDriverEndSyntheticMouse() {
    g_synthetic_mouse_active = false;
}

bool LookDriverSyntheticMouseActive() {
    return g_synthetic_mouse_active;
}

bool LookDriverHeadTrackingActive() {
    return g_head_valid && g_head_reference_valid;
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

void LookDriverSetHeadPose(bool valid, double yawDegrees, double pitchDegrees) {
    if (!valid) {
        if (!g_debug_head_path.empty() &&
            ::GetFileAttributesW(g_debug_head_path.c_str()) !=
                INVALID_FILE_ATTRIBUTES) {
            return;
        }
        if (g_head_valid) {
            H47VR_LOG_INFO("look driver: head tracking paused (no valid pose)");
        }
        g_head_valid = false;
        g_head_reference_valid = false;
        g_scene_pitch_degrees = 0.0;
        return;
    }

    if (!g_head_valid) {
        g_head_valid = true;
        H47VR_LOG_INFO("look driver: head tracking available");
    }
    if (!g_head_reference_valid) {
        g_head_reference_valid = true;
        g_head_reference_yaw = yawDegrees;
        g_head_reference_pitch = pitchDegrees;
        g_px_x = 0.0;
        g_px_y = 0.0;
        H47VR_LOG_INFO("look driver: head tracking recentered at yaw=%.2f "
                       "pitch=%.2f",
                       yawDegrees, pitchDegrees);
    }
    g_head_yaw = yawDegrees;
    g_head_pitch = pitchDegrees;
}

void LookDriverFrame(unsigned long frame) {
    if (frame % 30 == 0) {
        const bool nativePitch =
            !g_native_pitch_path.empty() &&
            ::GetFileAttributesW(g_native_pitch_path.c_str()) !=
                INVALID_FILE_ATTRIBUTES;
        if (nativePitch != g_native_pitch_enabled.exchange(nativePitch)) {
            H47VR_LOG_INFO("look driver: native MouseConCam pitch %s",
                           nativePitch ? "enabled" : "disabled");
        }
    }
    if (frame % 30 == 0) {
        WriteState();
    }
    double testYaw = 0.0;
    double testPitch = 0.0;
    if (ReadHeadPoseTest(&testYaw, &testPitch)) {
        if (!g_debug_head_active ||
            std::fabs(testYaw - g_last_debug_head_yaw) > 0.001 ||
            std::fabs(testPitch - g_last_debug_head_pitch) > 0.001) {
            g_debug_head_active = true;
            g_last_debug_head_yaw = testYaw;
            g_last_debug_head_pitch = testPitch;
            H47VR_LOG_INFO("look driver: head pose test override yaw=%.2f "
                           "pitch=%.2f",
                           testYaw, testPitch);
        }
        LookDriverSetHeadPose(true, testYaw, testPitch);
    } else if (g_debug_head_active) {
        g_debug_head_active = false;
        LookDriverSetHeadPose(false, 0.0, 0.0);
        H47VR_LOG_INFO("look driver: head pose test override disabled");
    }
    if (g_finish_frame != 0 && frame >= g_finish_frame) {
        if (!CameraReadbackGameplayActive() && frame < g_finish_deadline) {
            g_finish_frame = frame + 4;
            return;
        }
        g_finish_frame = 0;
        g_finish_deadline = 0;
        WriteState();
        ++g_turns;
        CameraReadback readback;
        double actualYaw = g_px_x * g_deg_per_px;
        double actualPitch = g_px_y * g_deg_per_px;
        const bool measured = ReadCurrentAim(&actualYaw, &actualPitch, &readback);
        g_refresh_after_readback = !measured;
        H47VR_LOG_INFO("look driver: turn complete (dx=%d dy=%d, %d message(s), "
                       "%lu posted so far, %lu failed, now yaw=%.2f pitch=%.2f "
                       "(%s), frame %lu)",
                       g_dx, g_dy, g_steps_total, g_messages.load(),
                       g_post_failures.load(), actualYaw, actualPitch,
                       measured ? "readback" : "estimate", frame);
    }

    if (g_refresh_after_readback) {
        if (CameraReadbackGameplayActive()) {
            g_refresh_after_readback = false;
            WriteState();
            H47VR_LOG_INFO("look driver: MouseConCam readback became active; "
                           "updated the settled aim state");
        }
    }

    TryPendingAbsolute(frame);

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
            g_px_x += g_dx;
            g_px_y += g_dy;
            g_deg_per_px = g_turn_deg_per_px;
            g_finish_frame = frame + 4;
            g_finish_deadline = frame + 180;
            H47VR_LOG_INFO("look driver: turn posted (dx=%d dy=%d), waiting for "
                           "the engine to settle",
                           g_dx, g_dy);
        }
        return;
    }

    ApplyHeadPose(frame);

    if (g_finish_frame != 0) {
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

double LookDriverScenePitchDegrees() {
    return g_scene_pitch_degrees.load();
}

void LookDriverReport(unsigned long frame) {
    if (!g_reported_window) {
        g_reported_window = true;
        H47VR_LOG_INFO("look driver: game window %p", WindowKeepaliveWindow());
    }
    H47VR_LOG_INFO("look driver: %lu turn(s), %lu mouse message(s) posted, %lu failed, "
                   "%lu cursor warp(s) swallowed, head tracking %s (%lu legacy, "
                   "%lu native correction(s), frame %lu)",
                   g_turns.load(), g_messages.load(), g_post_failures.load(),
                   g_warp_swallowed.load(), g_head_valid ? "active" : "off",
                   g_head_posts.load(), g_native_posts.load(), frame);
}

}  // namespace h47vr
