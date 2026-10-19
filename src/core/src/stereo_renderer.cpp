#include "h47vr/stereo_renderer.hpp"

#include "h47vr/gl_probe.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"

#include <windows.h>

#include <atomic>
#include <cstdlib>
#include <fstream>
#include <string>

namespace h47vr {
namespace {

std::wstring g_control_path;
std::atomic<bool> g_active{false};
std::atomic<float> g_eye_separation{10.0f};
std::atomic<unsigned long> g_world_draws{0};
std::atomic<unsigned long> g_stereo_passes{0};
unsigned int g_refresh_ticks = 0;
float g_loaded_separation = 10.0f;

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
    float separation = g_loaded_separation;
    const char* ipd = line.c_str();
    const char* prefix = "ipd=";
    if (line.compare(0, 4, prefix) == 0) {
        ipd = line.c_str() + 4;
        separation = static_cast<float>(std::atof(ipd));
    }
    if (separation <= 0.0f || separation > 1000.0f) {
        separation = 10.0f;
    }

    const bool wasActive = g_active.exchange(true);
    if (!wasActive || separation != g_loaded_separation) {
        g_loaded_separation = separation;
        g_eye_separation = separation;
        H47VR_LOG_INFO("stereo: on, side-by-side eye separation %.3f game units",
                       separation);
    }
}

}  // namespace

bool StereoRendererStart() {
    g_control_path = JoinPath(mod::GameDirectory(), L"h47vr-stereo.txt");
    H47VR_LOG_INFO("stereo renderer: ready - create %s with 'ipd=<game units>'",
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
    if (!GlStereoBeginEye(0, halfSeparation)) {
        draw(self, nullptr, list);
        return;
    }
    draw(self, nullptr, list);
    GlStereoEndEye();

    if (!GlStereoBeginEye(1, -halfSeparation)) {
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
