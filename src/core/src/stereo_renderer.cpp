#include "h47vr/stereo_renderer.hpp"

#include "h47vr/gl_probe.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/openxr_pose.hpp"
#include "h47vr/paths.hpp"

#include <windows.h>

#include <atomic>
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
std::atomic<bool> g_runtime_pose_seen{false};
unsigned int g_refresh_ticks = 0;
float g_loaded_separation = kDefaultIpdMillimetres /
                            (1000.0f / kDefaultGameUnitsPerMetre);
float g_loaded_ipd_mm = kDefaultIpdMillimetres;
float g_loaded_units_per_metre = kDefaultGameUnitsPerMetre;

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

}  // namespace

bool StereoRendererStart() {
    g_control_path = JoinPath(mod::GameDirectory(), L"h47vr-stereo.txt");
    H47VR_LOG_INFO("stereo renderer: ready - create %s with "
                   "'ipd_mm=<millimetres> units_per_m=<scale>'",
                   Narrow(g_control_path).c_str());
    return true;
}

void StereoRendererFrame(unsigned long frame) {
    RefreshStereo();
    if (g_active.load() && frame != 0 && frame % 600 == 0) {
        H47VR_LOG_INFO("stereo: %lu draw-list calls, %lu pairs rendered for both eyes "
                       "(frame %lu, eye separation %.3f)",
                       g_world_draws.load(), g_stereo_passes.load(), frame,
                       g_eye_separation.load());
    }
}

void StereoRendererDrawWorld(void* self, int* list, StereoDrawFn draw) {
    ++g_world_draws;
    RefreshStereo();

    if (draw == nullptr) {
        return;
    }
    if (!g_active.load() || !GlStereoAvailable() || !GlStereoPerspective()) {
        draw(self, nullptr, list);
        return;
    }

    const float halfSeparation = g_eye_separation.load() * 0.5f;
    float leftOffset = halfSeparation;
    float rightOffset = -halfSeparation;
    OpenXrEyePose runtimePose;
    if (OpenXrPoseGet(&runtimePose)) {
        // OpenXR X is positive to the right. The vertices are shifted the
        // opposite way, just as with the fixed-IPD fallback.
        leftOffset = -runtimePose.eyeXMetres[0] * g_loaded_units_per_metre;
        rightOffset = -runtimePose.eyeXMetres[1] * g_loaded_units_per_metre;
        if (!g_runtime_pose_seen.exchange(true)) {
            H47VR_LOG_INFO("stereo: using OpenXR eye positions %.4f / %.4f m "
                           "(offsets %.3f / %.3f game units)",
                           runtimePose.eyeXMetres[0], runtimePose.eyeXMetres[1],
                           leftOffset, rightOffset);
        }
    }

    if (!GlStereoBeginEye(0, leftOffset)) {
        draw(self, nullptr, list);
        return;
    }
    draw(self, nullptr, list);
    GlStereoEndEye();

    if (!GlStereoBeginEye(1, rightOffset)) {
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
