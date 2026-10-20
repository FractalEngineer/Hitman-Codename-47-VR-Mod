#include "h47vr/stereo_renderer.hpp"

#include "h47vr/camera_probe.hpp"
#include "h47vr/camera_readback.hpp"
#include "h47vr/gl_probe.hpp"
#include "h47vr/log.hpp"
#include "h47vr/look_driver.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/openxr_pose.hpp"
#include "h47vr/paths.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>

namespace h47vr {
namespace {

constexpr float kDefaultIpdMillimetres = 67.0f;
constexpr float kDefaultGameUnitsPerMetre = 100.0f;

std::wstring g_control_path;
std::atomic<bool> g_active{false};
std::atomic<float> g_eye_separation{kDefaultIpdMillimetres /
                                   (1000.0f / kDefaultGameUnitsPerMetre)};
std::atomic<unsigned long> g_world_draws{0};
std::atomic<unsigned long> g_stereo_passes{0};
std::atomic<unsigned long> g_theater_passes{0};
std::atomic<bool> g_runtime_pose_seen{false};
unsigned int g_refresh_ticks = 0;
float g_loaded_separation = kDefaultIpdMillimetres /
                            (1000.0f / kDefaultGameUnitsPerMetre);
float g_loaded_ipd_mm = kDefaultIpdMillimetres;
float g_loaded_units_per_metre = kDefaultGameUnitsPerMetre;
std::wstring g_state_path;
bool g_gameplay_active = false;
int g_last_mode = -1;

const char* ModeName(int mode) {
    switch (mode) {
        case 0: return "off";
        case 1: return "theater";
        case 2: return "stereo";
        default: return "?";
    }
}

void WriteState() {
    if (g_state_path.empty()) {
        return;
    }
    std::ofstream file(g_state_path, std::ios::trunc);
    if (!file) {
        return;
    }
    const int mode = !g_active.load() ? 0 : (g_gameplay_active ? 2 : 1);
    file << "mode=" << ModeName(mode)
         << " gameplay=" << (g_gameplay_active ? 1 : 0)
         << " stereo=" << (g_active.load() ? 1 : 0)
         << " eye_separation=" << g_eye_separation.load()
         << " stereo_passes=" << g_stereo_passes.load()
         << " theater_passes=" << g_theater_passes.load() << "\n";
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

void RefreshStereo() {
    if (g_control_path.empty() || (++g_refresh_ticks & 0x7F) != 0) {
        return;
    }

    const bool present =
        ::GetFileAttributesW(g_control_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!present) {
        if (g_active.exchange(false)) {
            H47VR_LOG_INFO("stereo: off (control file removed); %lu world draws, "
                           "%lu stereo passes",
                           g_world_draws.load(), g_stereo_passes.load());
        }
        return;
    }

    const std::string line = ReadFirstLine(g_control_path);
    float ipdMm = kDefaultIpdMillimetres;
    float unitsPerMetre = kDefaultGameUnitsPerMetre;
    float legacySeparation = 0.0f;
    ReadNumber(line, "ipd_mm", &ipdMm);
    ReadNumber(line, "units_per_m", &unitsPerMetre);
    const bool hasLegacySeparation = ReadNumber(line, "ipd", &legacySeparation);
    if (unitsPerMetre <= 1.0f || unitsPerMetre > 100000.0f) {
        unitsPerMetre = kDefaultGameUnitsPerMetre;
    }
    float separation = hasLegacySeparation
                           ? legacySeparation
                           : ipdMm * unitsPerMetre * 0.001f;
    if (separation <= 0.0f || separation > 1000.0f) {
        separation = kDefaultIpdMillimetres /
                     (1000.0f / kDefaultGameUnitsPerMetre);
    }

    const bool wasActive = g_active.exchange(true);
    if (!wasActive || separation != g_loaded_separation ||
        ipdMm != g_loaded_ipd_mm || unitsPerMetre != g_loaded_units_per_metre) {
        g_loaded_separation = separation;
        g_loaded_ipd_mm = ipdMm;
        g_loaded_units_per_metre = unitsPerMetre;
        g_eye_separation = separation;
        H47VR_LOG_INFO("stereo: on, IPD %.1f mm = %.3f game units "
                       "(%.1f game units/metre)",
                       ipdMm, separation, unitsPerMetre);
    }
}

void PublishRuntimeVisibilityUnion(const OpenXrEyePose& pose,
                                   float unitsPerMetre) {
    if (!pose.valid || !pose.fovValid || !(unitsPerMetre > 1.0f)) {
        CameraProbeSetRuntimeVisibilityFov(0.0);
        return;
    }

    double nearUnits = GlPerspectiveNear();
    if (!(nearUnits > 0.0)) {
        // The supported training camera uses a 5-unit near plane at the
        // current 100-units-per-metre calibration.
        nearUnits = 5.0 * static_cast<double>(unitsPerMetre) / 100.0;
    }

    int width = 0;
    int height = 0;
    GlStereoSourceSize(&width, &height);
    const double aspect =
        width > 0 && height > 0 ? static_cast<double>(width) / height
                                : 4.0 / 3.0;

    double tanLeft = 0.0;
    double tanRight = 0.0;
    double tanUp = 0.0;
    double tanDown = 0.0;
    for (int eye = 0; eye < 2; ++eye) {
        const double eyeX =
            static_cast<double>(pose.eyeXMetres[eye]) * unitsPerMetre;
        const double left =
            std::tan(static_cast<double>(pose.fov[eye][0])) * nearUnits;
        const double right =
            std::tan(static_cast<double>(pose.fov[eye][1])) * nearUnits;
        const double up =
            std::tan(static_cast<double>(pose.fov[eye][2])) * nearUnits;
        const double down =
            std::tan(static_cast<double>(pose.fov[eye][3])) * nearUnits;
        tanLeft = (std::min)(tanLeft, (eyeX + left) / nearUnits);
        tanRight = (std::max)(tanRight, (eyeX + right) / nearUnits);
        tanUp = (std::max)(tanUp, up / nearUnits);
        tanDown = (std::min)(tanDown, down / nearUnits);
    }

    const double horizontal =
        (std::max)(std::fabs(tanLeft), tanRight);
    const double vertical =
        (std::max)(tanUp, std::fabs(tanDown));
    // The engine derives vertical FOV from horizontal FOV using the current
    // desktop aspect. Pick the horizontal value that covers both axes.
    const double requiredTangent =
        (std::max)(horizontal, vertical * aspect);
    const double horizontalFov =
        2.0 * std::atan(requiredTangent);
    CameraProbeSetRuntimeVisibilityFov(horizontalFov);
}

}  // namespace

bool StereoRendererStart() {
    g_control_path = JoinPath(mod::GameDirectory(), L"h47vr-stereo.txt");
    g_state_path = JoinPath(mod::GameDirectory(), L"h47vr-stereo-state.txt");
    H47VR_LOG_INFO("stereo renderer: ready - create %s with "
                   "'ipd_mm=<millimetres> units_per_m=<scale>'",
                   Narrow(g_control_path).c_str());
    H47VR_LOG_INFO("stereo: gameplay state in %s",
                   Narrow(g_state_path).c_str());
    WriteState();
    return true;
}

void StereoRendererFrame(unsigned long frame) {
    RefreshStereo();
    OpenXrEyePose runtimePose;
    if (OpenXrPoseGet(&runtimePose)) {
        PublishRuntimeVisibilityUnion(runtimePose, g_loaded_units_per_metre);
    } else {
        CameraProbeSetRuntimeVisibilityFov(0.0);
    }
    const int mode = !g_active.load() ? 0 : (g_gameplay_active ? 2 : 1);
    if (mode != g_last_mode) {
        g_last_mode = mode;
        H47VR_LOG_INFO("stereo: presentation mode %s (gameplay=%s)",
                       ModeName(mode), g_gameplay_active ? "yes" : "no");
    }
    if (frame % 30 == 0) {
        WriteState();
    }
    if (g_active.load() && frame != 0 && frame % 600 == 0) {
        H47VR_LOG_INFO("stereo: %lu draw-list calls, %lu stereo pairs, %lu theater "
                       "passes (frame %lu, eye separation %.3f)",
                       g_world_draws.load(), g_stereo_passes.load(),
                       g_theater_passes.load(), frame,
                       g_eye_separation.load());
    }
}

StereoMode StereoRendererMode() {
    if (!g_active.load()) {
        return StereoMode::Off;
    }
    return g_gameplay_active ? StereoMode::Stereo : StereoMode::Theater;
}

void StereoRendererDrawWorld(void* self, int* list, StereoDrawFn draw) {
    ++g_world_draws;
    RefreshStereo();

    if (draw == nullptr) {
        return;
    }
    g_gameplay_active = CameraReadbackGameplayActive();
    if (!g_active.load() || !g_gameplay_active || !GlStereoAvailable() ||
        !GlStereoPerspective()) {
        if (g_active.load()) {
            ++g_theater_passes;
        }
        draw(self, nullptr, list);
        return;
    }

    const float halfSeparation = g_eye_separation.load() * 0.5f;
    float leftOffset = halfSeparation;
    float rightOffset = -halfSeparation;
    const float* leftFov = nullptr;
    const float* rightFov = nullptr;
    OpenXrStereoTarget directTarget;
    bool directStereo = OpenXrBeginStereoTarget(&directTarget);
    OpenXrEyePose runtimePose;
    float sceneRollRadians = 0.0f;
    if (OpenXrPoseGet(&runtimePose)) {
        // OpenXR X is positive to the right. The vertices are shifted the
        // opposite way, just as with the fixed-IPD fallback.
        leftOffset = -runtimePose.eyeXMetres[0] * g_loaded_units_per_metre;
        rightOffset = -runtimePose.eyeXMetres[1] * g_loaded_units_per_metre;
        if (runtimePose.fovValid) {
            leftFov = runtimePose.fov[0];
            rightFov = runtimePose.fov[1];
        }
        sceneRollRadians =
            runtimePose.rollDegrees * 0.017453292519943295f;
        if (!g_runtime_pose_seen.exchange(true)) {
            H47VR_LOG_INFO("stereo: using OpenXR eye positions %.4f / %.4f m "
                           "(offsets %.3f / %.3f game units), runtime asymmetric FOV %s",
                           runtimePose.eyeXMetres[0], runtimePose.eyeXMetres[1],
                           leftOffset, rightOffset,
                           runtimePose.fovValid ? "enabled" : "unavailable");
        }
        PublishRuntimeVisibilityUnion(runtimePose, g_loaded_units_per_metre);
    } else {
        CameraProbeSetRuntimeVisibilityFov(0.0);
    }

    // Desktop diagnostic for the headset culling path: h47vr-widefov.txt gives
    // a horizontal FOV in degrees, which is turned into the same asymmetric
    // per-eye projection OpenXR supplies. This exercises clip bypass and the
    // widened final projection together without requiring a headset.
    float diagnosticFov[2][4] = {};
    const double diagnosticHorizontal =
        CameraProbeWideFovRadians();
    if (!runtimePose.fovValid && diagnosticHorizontal > 0.0) {
        int width = 0;
        int height = 0;
        GlStereoSourceSize(&width, &height);
        const double aspect =
            width > 0 && height > 0 ? static_cast<double>(width) / height
                                    : 4.0 / 3.0;
        const double horizontalHalf = diagnosticHorizontal * 0.5;
        const double verticalHalf =
            std::atan(std::tan(horizontalHalf) / aspect);
        for (int eye = 0; eye < 2; ++eye) {
            diagnosticFov[eye][0] = static_cast<float>(-horizontalHalf);
            diagnosticFov[eye][1] = static_cast<float>(horizontalHalf);
            diagnosticFov[eye][2] = static_cast<float>(verticalHalf);
            diagnosticFov[eye][3] = static_cast<float>(-verticalHalf);
        }
        leftFov = diagnosticFov[0];
        rightFov = diagnosticFov[1];
    }

    if (!GlStereoBeginEye(
            0, leftOffset, leftFov, sceneRollRadians,
            static_cast<float>(LookDriverScenePitchDegrees() *
                               0.017453292519943295),
            directStereo ? directTarget.framebuffer[0] : 0,
            directStereo ? directTarget.width : 0,
            directStereo ? directTarget.height : 0)) {
        if (directStereo) {
            OpenXrCancelStereoTarget();
            directStereo = false;
        }
        draw(self, nullptr, list);
        return;
    }
    draw(self, nullptr, list);
    GlStereoEndEye();

    if (!GlStereoBeginEye(
            1, rightOffset, rightFov, sceneRollRadians,
            static_cast<float>(LookDriverScenePitchDegrees() *
                               0.017453292519943295),
            directStereo ? directTarget.framebuffer[1] : 0,
            directStereo ? directTarget.width : 0,
            directStereo ? directTarget.height : 0)) {
        if (directStereo) {
            OpenXrCancelStereoTarget();
        }
        return;
    }
    draw(self, nullptr, list);
    GlStereoEndEye();

    const unsigned long passes = ++g_stereo_passes;
    if (passes == 1) {
        H47VR_LOG_INFO("stereo: %lu perspective passes rendered for both eyes "
                       "(eye separation %.3f)",
                       passes, g_eye_separation.load());
    }
}

}  // namespace h47vr
