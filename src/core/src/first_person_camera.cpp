#include "h47vr/first_person_camera.hpp"

#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>

namespace h47vr {
namespace {

// The supported HitmanDlc.dlc build contains:
//   0x2714f: mov dword ptr [esi+0x55], 0x43160000  ; 150.0
//   0x27158: mov dword ptr [esi+0x55], 0x42c80000  ; 100.0
// Both are MouseConCam's target follow distance. The immediates start three
// bytes into each instruction; refusal on a changed opcode keeps this patch
// honest if the installed build ever differs from the pins.
constexpr unsigned long kNormalImmediateRva = 0x27152;
constexpr unsigned long kLeanImmediateRva = 0x2715B;
constexpr float kOriginalNormalDistance = 150.0f;
constexpr float kOriginalLeanDistance = 100.0f;
constexpr float kDefaultFirstPersonDistance = 3.0f;

std::wstring g_control_path;
bool g_installed = false;
bool g_active = false;
float g_distance = 0.0f;
unsigned int g_refresh_ticks = 0;

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

bool ReadDistance(const std::string& line, float* distance) {
    const std::string marker = "distance=";
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
    *distance = parsed;
    return true;
}

bool PatchImmediate(std::uintptr_t module, unsigned long rva, float value,
                    const char* name) {
    auto* instruction = reinterpret_cast<unsigned char*>(module + rva - 3);
    if (instruction[0] != 0xC7 || instruction[1] != 0x46 ||
        instruction[2] != 0x55) {
        H47VR_LOG_ERROR("first person: %s instruction mismatch at %p "
                        "(expected c7 46 55, found %02x %02x %02x)",
                        name, instruction, instruction[0], instruction[1],
                        instruction[2]);
        return false;
    }

    DWORD oldProtect = 0;
    auto* immediate = reinterpret_cast<float*>(module + rva);
    if (!::VirtualProtect(immediate, sizeof(value), PAGE_EXECUTE_READWRITE,
                          &oldProtect)) {
        H47VR_LOG_ERROR("first person: VirtualProtect failed for %s at %p "
                        "(error %lu)",
                        name, immediate, ::GetLastError());
        return false;
    }
    std::memcpy(immediate, &value, sizeof(value));
    ::FlushInstructionCache(::GetCurrentProcess(), instruction, 7);
    DWORD ignored = 0;
    ::VirtualProtect(immediate, sizeof(value), oldProtect, &ignored);
    return true;
}

void ApplyDistance(float distance) {
    const HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    if (gameplay == nullptr) {
        return;
    }
    const auto module = reinterpret_cast<std::uintptr_t>(gameplay);
    const bool normal =
        PatchImmediate(module, kNormalImmediateRva, distance, "normal");
    const bool lean =
        PatchImmediate(module, kLeanImmediateRva, distance, "lean");
    if (normal && lean) {
        H47VR_LOG_INFO("first person: MouseConCam target distance %.3f game "
                       "units (powered by the engine's own camera transform)",
                       distance);
    }
}

void RestoreFollowDistance() {
    const HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    if (gameplay == nullptr) {
        return;
    }
    const auto module = reinterpret_cast<std::uintptr_t>(gameplay);
    const bool normal = PatchImmediate(
        module, kNormalImmediateRva, kOriginalNormalDistance, "normal restore");
    const bool lean = PatchImmediate(
        module, kLeanImmediateRva, kOriginalLeanDistance, "lean restore");
    if (normal && lean) {
        H47VR_LOG_INFO("first person: off; MouseConCam follow distance restored");
    }
}

}  // namespace

bool FirstPersonCameraStart() {
    g_control_path =
        JoinPath(mod::GameDirectory(), L"h47vr-firstperson.txt");
    H47VR_LOG_INFO("first person camera: ready - create %s with "
                   "'distance=<game units>' (default 3.0, off by default)",
                   Narrow(g_control_path).c_str());
    FirstPersonCameraRearm();
    return true;
}

void FirstPersonCameraFrame(unsigned long) {
    if (g_control_path.empty() || (++g_refresh_ticks & 0x0F) != 0) {
        return;
    }

    const bool present =
        ::GetFileAttributesW(g_control_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!present) {
        if (g_active) {
            g_active = false;
            RestoreFollowDistance();
        }
        return;
    }

    const std::string line = ReadFirstLine(g_control_path);
    float distance = kDefaultFirstPersonDistance;
    ReadDistance(line, &distance);
    if (distance < 0.25f || distance > 149.0f) {
        if (g_active) {
            H47VR_LOG_WARN("first person: invalid distance %.3f; disabling",
                           distance);
            g_active = false;
            RestoreFollowDistance();
        }
        return;
    }

    if (!g_active || distance != g_distance) {
        g_active = true;
        g_distance = distance;
        ApplyDistance(distance);
    }
}

void FirstPersonCameraRearm() {
    g_installed = LoadedModule(L"HitmanDlc.dlc") != nullptr;
    if (!g_installed) {
        return;
    }
    if (g_active) {
        ApplyDistance(g_distance);
    }
}

bool FirstPersonCameraActive() {
    return g_active;
}

}  // namespace h47vr
