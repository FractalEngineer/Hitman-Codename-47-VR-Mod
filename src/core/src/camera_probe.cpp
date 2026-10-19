#include "h47vr/camera_probe.hpp"

#include "h47vr/hooks.hpp"
#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"

#include <windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace h47vr {

void DumpCamera(void* camera, const char* why);

namespace {

// `FUN_0fd53c50` is the ZGEOM `Clip` method: it builds and visibility-tests the
// draw list for one group of world geometry, and it is where the world vertices
// are submitted (`glVertex3fv` is reached from inside it). Its last stack
// argument is the *geometry object*, not the camera - live capture shows a
// different pointer on every call - so it is only a witness that the world path
// ran, and a counter for how much geometry a frame contains.
constexpr unsigned long kClipRva = 0x00093C50;

// `FUN_0fd56120` is ZCamera.cpp's `CamUpdate`, the per-frame world draw entry.
// It is the engine frame on both the `glVertex3fv` stack and the `glFrustum`
// stack. `ECX` differs per call (it runs per geometry group) but its first stack
// argument is stable, so that one is the scene/render object.
//
// The important detail, read out of the machine code rather than the
// decompiler: CamUpdate stores its own `ECX` into the scratch object it hands to
// `Clip` (`mov [ebp-0x2018], esi` at 0xfd56279), and Clip then reads three
// floats from that object - `[ecx+0x106]`, `[ecx+0x10a]`, `[ecx+0x10e]` - and
// uses them as a rotation. So whatever CamUpdate runs on is camera-shaped.
constexpr unsigned long kCameraUpdateRva = 0x00096120;

// How much of a candidate object to watch. The interesting fields sit early, but
// the first attempt at 0x400 bytes found nothing rotation-shaped in them, so the
// window is doubled and the *diff* across frames is what does the real work.
constexpr int kScanBytes = 0x800;

extern "C" void* h47vr_camera_trampoline = nullptr;
extern "C" void* h47vr_camera_args_target = nullptr;

extern "C" void* h47vr_update_trampoline = nullptr;
extern "C" void* h47vr_update_args_target = nullptr;
// ECX at entry to CamUpdate, saved before anything else can touch it.
extern "C" void* h47vr_update_ecx = nullptr;

std::atomic<unsigned long> g_clip_calls{0};
std::atomic<void*> g_scene_object{nullptr};
std::atomic<unsigned long> g_update_calls{0};
std::wstring g_gate;

// Entry layout: [esp] = return address, ECX = this, stack arguments from
// [esp+4]. The thunk preserves every register and tail-jumps to the original
// bytes, so it assumes nothing about the convention or the argument count - it
// only reads the arguments, and the original `ret` still does the cleanup.
extern "C" __declspec(naked) void h47vr_detour_camera_transform() {
    __asm {
        pushfd
        pushad
        // After pushfd+pushad: eflags is at [esp+32], the return address at
        // [esp+36] and the stack arguments from [esp+40] upward. Pushing the
        // same slot four times walks the arguments out in reverse order,
        // because each push moves the next one up by four.
        push dword ptr [esp+52]
        push dword ptr [esp+52]
        push dword ptr [esp+52]
        push dword ptr [esp+52]
        call dword ptr [h47vr_camera_args_target]
        add esp, 16
        popad
        popfd
        jmp dword ptr [h47vr_camera_trampoline]
    }
}

// CamUpdate takes its object in ECX and two more arguments on the stack. ECX is
// saved first, before pushad can hide it, and the thunk still tail-jumps to the
// original bytes so the calling convention stays the engine's problem.
extern "C" __declspec(naked) void h47vr_detour_camera_update() {
    __asm {
        mov dword ptr [h47vr_update_ecx], ecx
        pushfd
        pushad
        // After pushfd+pushad: eflags at [esp+32], return address at [esp+36],
        // the first stack argument at [esp+40] and the second at [esp+44].
        // Pushing [esp+44] twice walks them out in reverse order.
        push dword ptr [esp+44]
        push dword ptr [esp+44]
        push dword ptr [h47vr_update_ecx]
        call dword ptr [h47vr_update_args_target]
        add esp, 12
        popad
        popfd
        jmp dword ptr [h47vr_update_trampoline]
    }
}

// Committed, readable pages only. Every pointer here comes out of live engine
// state, so it is worth one cheap check before dereferencing it.
bool Readable(const void* address, size_t bytes) {
    if (address == nullptr) {
        return false;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (::VirtualQuery(address, &info, sizeof(info)) == 0) {
        return false;
    }
    if (info.State != MEM_COMMIT) {
        return false;
    }
    constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                                PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                                PAGE_EXECUTE_WRITECOPY;
    if ((info.Protect & kReadable) == 0) {
        return false;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    return start + bytes <= end;
}

// --- finding the camera by watching memory change ---------------------------
//
// The decompiler mis-reads this code's stack arguments (that is how the last
// argument of Clip was mistaken for a camera), and two rounds of "read the
// decompilation, guess the object" both went wrong. So stop guessing.
//
// The camera is defined by what it does: it moves when the view moves, while the
// rest of the scene does not. Every object the engine runs CamUpdate on is
// therefore a candidate, and the test is a memory diff between snapshots taken a
// couple of seconds apart *while the opening cutscene is sweeping the camera*.
// The object whose transform fields change is the camera, and the offsets that
// change are the fields to drive.
struct Candidate {
    void* object;
    bool baseline;
    unsigned char snapshot[kScanBytes];
    // The triple `Clip` reads as a rotation, kept so a change can be reported
    // rather than repeated. If this is the view rotation, it is unit length and
    // it turns when the view turns.
    float triple[3];
    bool tripleSeen;
    // A camera is the only thing here whose position is far from the origin, so
    // the first triple with a large magnitude is worth dumping around: that is
    // where its rotation will be, if it stores one.
    bool posDumped;
    // The 3x3 at +0x0DA, which is where every camera-shaped object here keeps
    // its basis: the level camera holds the plain axis swap there and the menu
    // camera holds a real rotation. If one of these turns when the mouse turns,
    // that object is the camera and this is the field to drive.
    float basis[9];
    bool basisSeen;
    // What this probe last wrote here, so the engine's own value can be told
    // apart from ours. Without this, a second write would rotate the result of
    // the first and the view would spin away a little more every frame.
    float written[9];
    bool wrote;
    // True once the engine itself has changed this object's basis. That is what
    // marks it as the camera the mouse drives rather than a frozen copy of it.
    bool basisChanged;
    // True once the +0x106 triple has been seen to move. Several objects hold a
    // constant 8000 or 1000 at that offset, which are far-plane and near-plane
    // style constants; the camera is the object whose triple keeps moving.
    bool tripleChanged;
    // Per-offset change counters over the window before the next report, so the
    // fields the engine drives on the camera can be named instead of guessed at.
    // Only candidates that have already shown themselves to be the camera are
    // tracked this way.
    unsigned char prev[kScanBytes];
    bool prevValid;
    unsigned short changes[kScanBytes / 4];
};

constexpr int kMaxCandidates = 24;
Candidate g_candidates[kMaxCandidates] = {};
int g_candidate_count = 0;

// Camera-shaped 3x3 blocks, logged once per candidate for completeness. Both
// layouts are checked because the module's own property schema declares the
// ZGEOM transform as `double Position[3]` / `double Matrix[3][3]`, while the
// per-instance math this project has read is all `float`.
bool Finite(float v) {
    return v == v && v > -1.0e30f && v < 1.0e30f;
}

bool UnitTriple(float x, float y, float z) {
    const float length = std::sqrt(x * x + y * y + z * z);
    return length > 0.98f && length < 1.02f;
}

bool OrthonormalF(const float* m) {
    for (int row = 0; row < 3; ++row) {
        const float* r = m + row * 3;
        if (!Finite(r[0]) || !Finite(r[1]) || !Finite(r[2]) ||
            !UnitTriple(r[0], r[1], r[2])) {
            return false;
        }
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            const float* ra = m + a * 3;
            const float* rb = m + b * 3;
            const float dot = ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2];
            if (dot < -0.03f || dot > 0.03f) {
                return false;
            }
        }
    }
    return true;
}

bool OrthonormalD(const double* m) {
    for (int row = 0; row < 3; ++row) {
        const double* r = m + row * 3;
        if (!Finite(static_cast<float>(r[0])) || !Finite(static_cast<float>(r[1])) ||
            !Finite(static_cast<float>(r[2]))) {
            return false;
        }
        const double length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
        if (length <= 0.98 || length >= 1.02) {
            return false;
        }
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            const double* ra = m + a * 3;
            const double* rb = m + b * 3;
            const double dot = ra[0] * rb[0] + ra[1] * rb[1] + ra[2] * rb[2];
            if (dot < -0.03 || dot > 0.03) {
                return false;
            }
        }
    }
    return true;
}

// Rotations already reported, so a repeated scan does not repeat the log. The
// first scan of a freshly created object finds nothing (its fields are still
// zero), so the scan has to be repeated after the engine has filled it in.
struct FoundRotation {
    void* object;
    int offset;
    bool isDouble;
};

constexpr int kMaxFound = 64;
FoundRotation g_found[kMaxFound] = {};
int g_found_count = 0;

bool AlreadyReported(void* object, int offset, bool isDouble) {
    for (int i = 0; i < g_found_count; ++i) {
        if (g_found[i].object == object && g_found[i].offset == offset &&
            g_found[i].isDouble == isDouble) {
            return true;
        }
    }
    if (g_found_count < kMaxFound) {
        g_found[g_found_count++] = {object, offset, isDouble};
    }
    return false;
}

// Logs every rotation-shaped 3x3 in the object. The tolerance is loose because
// the engine's matrices are not guaranteed orthonormal to the last bit.
void ScanForRotations(void* object) {
    if (!Readable(object, kScanBytes)) {
        return;
    }
    const auto* base = static_cast<const unsigned char*>(object);
    int found = 0;
    // Step by 2, not 4: the camera's own position sits at +0x106, which is not
    // 4-byte aligned, so whatever follows it is not either. Scanning only
    // 4-aligned offsets silently skipped exactly the block being looked for.
    for (int offset = 0; offset + 9 * static_cast<int>(sizeof(double)) <= kScanBytes;
         offset += 2) {
        if (found >= 4) {
            break;
        }
        const auto* f = reinterpret_cast<const float*>(base + offset);
        if (OrthonormalF(f)) {
            if (AlreadyReported(object, offset, false)) {
                continue;
            }
            H47VR_LOG_INFO("  %p rotation(float)  at +0x%03X | %7.3f %7.3f %7.3f | "
                           "%7.3f %7.3f %7.3f | %7.3f %7.3f %7.3f |",
                           object, offset, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8]);
            ++found;
            continue;
        }
        if ((offset & 7) == 0) {
            const auto* d = reinterpret_cast<const double*>(base + offset);
            if (OrthonormalD(d)) {
                if (AlreadyReported(object, offset, true)) {
                    continue;
                }
                H47VR_LOG_INFO("  %p rotation(double) at +0x%03X | %7.3f %7.3f %7.3f | "
                               "%7.3f %7.3f %7.3f | %7.3f %7.3f %7.3f |",
                               object, offset, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7], d[8]);
                ++found;
            }
        }
    }
}

// Registers an object to watch, once. Returns true when this call added it.
bool AddCandidate(void* object) {
    if (object == nullptr || !Readable(object, kScanBytes)) {
        return false;
    }
    for (int i = 0; i < g_candidate_count; ++i) {
        if (g_candidates[i].object == object) {
            return false;
        }
    }
    if (g_candidate_count >= kMaxCandidates) {
        return false;
    }
    Candidate& c = g_candidates[g_candidate_count++];
    c.object = object;
    c.baseline = false;
    c.tripleSeen = false;
    c.posDumped = false;
    c.basisSeen = false;
    std::memset(c.basis, 0, sizeof(c.basis));
    c.wrote = false;
    std::memset(c.written, 0, sizeof(c.written));
    c.basisChanged = false;
    c.tripleChanged = false;
    c.prevValid = false;
    std::memset(c.prev, 0, sizeof(c.prev));
    std::memset(c.changes, 0, sizeof(c.changes));
    c.triple[0] = c.triple[1] = c.triple[2] = 0.0f;
    std::memset(c.snapshot, 0, sizeof(c.snapshot));
    return true;
}

// Reads the triple at +0x106, the one Clip consumes. Reports it when it changes,
// with its length: a view rotation is unit length, a colour or a scale is not.
void ReportTriple(Candidate& c) {
    const auto* f = reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(c.object) + 0x106);
    if (!Readable(f, 12)) {
        return;
    }
    const float x = f[0];
    const float y = f[1];
    const float z = f[2];
    const bool changed = !c.tripleSeen || x != c.triple[0] || y != c.triple[1] ||
                         z != c.triple[2];
    if (!changed) {
        return;
    }
    if (c.tripleSeen) {
        c.tripleChanged = true;
    }
    c.tripleSeen = true;
    c.triple[0] = x;
    c.triple[1] = y;
    c.triple[2] = z;
    const float length = std::sqrt(x * x + y * y + z * z);
    H47VR_LOG_INFO("  %p +0x106 = %9.4f %9.4f %9.4f   |v| = %.4f",
                   c.object, x, y, z, length);

    // A world position, and nowhere near the origin: this is the camera, or the
    // thing the camera follows. Dump around it once so its layout is visible -
    // the rotation, if it is stored rather than derived, is in this window.
    if (length > 500.0f && !c.posDumped) {
        c.posDumped = true;
        const auto* base = static_cast<const unsigned char*>(c.object);
        // The direct guess, spelled out: position first, then the nine floats
        // that would follow it if the object stores a 3x3 after its position.
        const auto* m = reinterpret_cast<const float*>(base + 0x112);
        if (Readable(m, 9 * sizeof(float))) {
            H47VR_LOG_INFO("    +0x112 (as a 3x3) | %8.4f %8.4f %8.4f | %8.4f %8.4f "
                           "%8.4f | %8.4f %8.4f %8.4f |",
                           m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
        }
        // Raw words, not floats: the layout here is not 4-aligned, so the only
        // honest view of it is hex. The position triple at +0x106 is visible as
        // three words starting at that offset.
        for (int row = 0; row < 3; ++row) {
            const int at = 0x0F0 + row * 0x20;
            const auto* w = reinterpret_cast<const unsigned int*>(base + at);
            if (!Readable(w, 8 * sizeof(unsigned int))) {
                break;
            }
            H47VR_LOG_INFO("    +0x%03X  %08X %08X %08X %08X %08X %08X %08X %08X",
                           at, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
        }
    }
}

// The 3x3 at +0x0DA, logged whenever it changes. This is the field that tells the
// camera apart from everything else: turn the view and only its basis moves.
void ReportBasis(Candidate& c) {
    const auto* m = reinterpret_cast<const float*>(
        static_cast<const unsigned char*>(c.object) + 0x0DA);
    if (!Readable(m, 9 * sizeof(float))) {
        return;
    }
    bool changed = !c.basisSeen;
    for (int i = 0; i < 9 && !changed; ++i) {
        if (m[i] != c.basis[i]) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    if (c.basisSeen) {
        // The engine moved it: this is the live camera, not a frozen copy.
        c.basisChanged = true;
    }
    c.basisSeen = true;
    for (int i = 0; i < 9; ++i) {
        c.basis[i] = m[i];
    }
    H47VR_LOG_INFO("  %p +0x0DA | %8.4f %8.4f %8.4f | %8.4f %8.4f %8.4f | "
                   "%8.4f %8.4f %8.4f |",
                   c.object, m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8]);
}

// --- driving the engine's own camera ---------------------------------------
//
// h47vr-camwrite.txt:  yaw=<deg> pitch=<deg> [at=frame|frustum]
//
// The empirical answer to "which object is the camera" is the one whose basis
// the engine itself turns: sweeping the mouse turned exactly one candidate's
// +0x0DA and nothing else in the level. So the write goes only to candidates the
// engine has been observed updating - a frozen copy of the same basis exists
// elsewhere, and rotating that would do nothing visible.
//
// The write is deliberately idempotent. The engine's own value is adopted and
// rotated; if what is in memory is still our value, nothing happens, so calling
// this every frame cannot compound into a spin.
std::wstring g_write_path;
std::atomic<bool> g_write_open{false};
std::atomic<bool> g_write_from_frustum{false};
std::atomic<bool> g_write_dry{false};
// -1 = the camera picked by its behaviour; >= 0 = one specific candidate, for
// the sweep that finds which object the renderer actually reads.
std::atomic<int> g_write_index{-1};
constexpr bool kWriteAll = false;
float g_write_yaw = 0.0f;
float g_write_pitch = 0.0f;
unsigned int g_write_ticks = 0;
std::atomic<unsigned long> g_write_applied{0};

// Rx(pitch) * Ry(yaw), row-major, the same composition the vertex-level camera
// uses. Pre-multiplied onto the camera's basis, this rotates the camera itself,
// which is what makes the culling follow the view.
void ShapeWriteMatrix(float out[9]) {
    const float yaw = g_write_yaw * 3.14159265358979f / 180.0f;
    const float pitch = g_write_pitch * 3.14159265358979f / 180.0f;
    const float cy = std::cos(yaw);
    const float sy = std::sin(yaw);
    const float cp = std::cos(pitch);
    const float sp = std::sin(pitch);
    const float yawM[9] = {cy, 0.0f, sy, 0.0f, 1.0f, 0.0f, -sy, 0.0f, cy};
    const float pitchM[9] = {1.0f, 0.0f, 0.0f, 0.0f, cp, -sp, 0.0f, sp, cp};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += pitchM[row * 3 + k] * yawM[k * 3 + column];
            }
            out[row * 3 + column] = sum;
        }
    }
}

// Re-reads the control file, a few times a second rather than every frame, so a
// live edit takes effect without a relaunch.
void RefreshCameraWrite() {
    if ((++g_write_ticks & 0x1F) != 0) {
        return;
    }
    const bool present =
        ::GetFileAttributesW(g_write_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!present) {
        if (g_write_open.exchange(false)) {
            H47VR_LOG_INFO("camera write: off (control file removed), %lu writes",
                           g_write_applied.load());
        }
        return;
    }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, g_write_path.c_str(), L"rb") != 0 || file == nullptr) {
        return;
    }
    char text[160] = {};
    const size_t read = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[read] = '\0';

    const float yaw = static_cast<float>(std::atof(std::strstr(text, "yaw=") ? std::strstr(text, "yaw=") + 4 : "0"));
    const float pitch =
        static_cast<float>(std::atof(std::strstr(text, "pitch=") ? std::strstr(text, "pitch=") + 6 : "0"));
    const bool fromFrustum = std::strstr(text, "at=frustum") != nullptr;
    const bool dry = std::strstr(text, "dry=1") != nullptr;
    int index = -1;
    const char* indexAt = std::strstr(text, "index=");
    if (indexAt != nullptr) {
        index = std::atoi(indexAt + 6);
    }

    const bool wasOpen = g_write_open.exchange(true);
    if (!wasOpen || yaw != g_write_yaw || pitch != g_write_pitch ||
        fromFrustum != g_write_from_frustum.load() || dry != g_write_dry.load() ||
        index != g_write_index.load()) {
        g_write_yaw = yaw;
        g_write_pitch = pitch;
        g_write_from_frustum = fromFrustum;
        g_write_dry = dry;
        g_write_index = index;
        H47VR_LOG_INFO("camera write: on (yaw=%.2f pitch=%.2f at=%s%s)", yaw, pitch,
                       fromFrustum ? "frustum" : "frame",
                       index >= 0 ? " (single candidate)" : (dry ? ", dry run" : ""));
    }
}

void ApplyCameraWrite() {
    if (!g_write_open.load()) {
        return;
    }
    float rotation[9];
    ShapeWriteMatrix(rotation);

    const bool dry = g_write_dry.load();
    const int only = g_write_index.load();
    int touched = 0;
    for (int i = 0; i < g_candidate_count; ++i) {
        if (touched >= 4) {
            break;  // a camera is one object; more than a few means a mistake
        }
        if (only >= 0 && i != only) {
            continue;
        }
        Candidate& c = g_candidates[i];
        if (!c.basisSeen) {
            continue;
        }
        auto* m = reinterpret_cast<float*>(
            static_cast<unsigned char*>(c.object) + 0x0DA);
        if (!Readable(m, 9 * sizeof(float))) {
            continue;
        }
        // Only a genuine rotation is ever touched. The first version of this
        // writer skipped this check, and when a world object happened to hold
        // non-float data at +0x0DA it overwrote it with nonsense and took the
        // game down. A camera basis is orthonormal; nothing else here is written.
        if (!OrthonormalF(m)) {
            continue;
        }
        // The camera, and only the camera, is picked by its position rather than
        // by its basis. Its basis at the start of the training level is exactly
        // the ZGEOM default (the axis swap) because the player faces along an
        // axis, so filtering on "not the default" threw the camera away. What
        // nothing else here does is keep moving: several candidates hold a
        // constant 8000 or 1000 at +0x106, which never change.
        if (only < 0 && !c.tripleChanged && !c.basisChanged) {
            continue;
        }
        if (c.wrote && std::memcmp(m, c.written, sizeof(c.written)) == 0) {
            continue;  // still our value; the engine has not refreshed it
        }
        float next[9];
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                float sum = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    sum += rotation[row * 3 + k] * m[k * 3 + column];
                }
                next[row * 3 + column] = sum;
            }
        }
        std::memcpy(c.written, next, sizeof(next));
        ++touched;
        if (dry) {
            if (g_write_applied.load() == 0) {
                H47VR_LOG_INFO("camera write (dry): would write %p | %8.4f %8.4f %8.4f | "
                               "%8.4f %8.4f %8.4f | %8.4f %8.4f %8.4f |",
                               c.object, next[0], next[1], next[2], next[3], next[4],
                               next[5], next[6], next[7], next[8]);
                g_write_applied = 1;
            }
            continue;
        }
        c.wrote = true;
        std::memcpy(m, next, sizeof(next));
        const unsigned long total = ++g_write_applied;
        if (total == 1 || total % 300 == 0) {
            H47VR_LOG_INFO("camera write: %p basis | %8.4f %8.4f %8.4f | "
                           "%8.4f %8.4f %8.4f | %8.4f %8.4f %8.4f |",
                           c.object, next[0], next[1], next[2], next[3], next[4],
                           next[5], next[6], next[7], next[8]);
        }
    }
}

// Diffs one candidate against its snapshot and refreshes it, returning how many
// dwords moved. The per-dword detail is deliberately not logged here: at 2 s
// apart almost everything in an animated level moves, and the useful question is
// which single object is the camera, not what the scenery is doing.
int DiffCandidate(Candidate& c) {
    if (!Readable(c.object, kScanBytes)) {
        return 0;
    }
    const auto* now = static_cast<const unsigned char*>(c.object);
    if (!c.baseline) {
        std::memcpy(c.snapshot, now, kScanBytes);
        c.baseline = true;
        return 0;
    }

    int changed = 0;
    for (int offset = 0; offset + 4 <= kScanBytes; offset += 4) {
        if (std::memcmp(c.snapshot + offset, now + offset, 4) == 0) {
            continue;
        }
        ++changed;
        std::memcpy(c.snapshot + offset, now + offset, 4);
    }
    return changed;
}

void ObserveCameraTransform(void* a1, void* a2, void* a3, void* a4) {
    ++g_clip_calls;
}

void ObserveCameraUpdate(void* self, void* a1, void* a2) {
    const unsigned long call = ++g_update_calls;

    // The stable first stack argument is the scene/render object; the `this`
    // object is a camera candidate. Both are worth watching.
    if (g_scene_object.load() == nullptr && Readable(a1, kScanBytes)) {
        g_scene_object = a1;
        H47VR_LOG_INFO("cam update: scene object is %p (arg1, stable across calls)", a1);
        if (AddCandidate(a1)) {
            H47VR_LOG_INFO("  watching scene object %p; rotation-shaped blocks:", a1);
            ScanForRotations(a1);
        }
    }

    if (call <= 6) {
        H47VR_LOG_INFO("cam update #%lu self(ecx)=%p arg1=%p arg2=%p",
                       call, self, a1, a2);
    }

    if (AddCandidate(self)) {
        H47VR_LOG_INFO("cam update: watching new self #%d %p (call %lu); "
                       "rotation-shaped blocks:", g_candidate_count - 1, self, call);
        ScanForRotations(self);
        const auto* f = reinterpret_cast<const float*>(
            static_cast<const unsigned char*>(self) + 0x106);
        H47VR_LOG_INFO("  [self+0x106] = %.4f %.4f %.4f", f[0], f[1], f[2]);
    }
}

}  // namespace

void* CameraProbeObject() {
    return g_scene_object.load();
}

bool CameraProbeInstall() {
    HMODULE gameplay = LoadedModule(L"HitmanDlc.dlc");
    if (gameplay == nullptr) {
        H47VR_LOG_WARN("camera probe: HitmanDlc.dlc is not loaded");
        return false;
    }

    g_gate = JoinPath(mod::GameDirectory(), L"h47vr-camobj.txt");
    g_write_path = JoinPath(mod::GameDirectory(), L"h47vr-camwrite.txt");
    h47vr_camera_args_target = reinterpret_cast<void*>(&ObserveCameraTransform);

    void* target = reinterpret_cast<char*>(gameplay) + kClipRva;
    if (!HookAddress(target, reinterpret_cast<void*>(&h47vr_detour_camera_transform),
                     &h47vr_camera_trampoline)) {
        H47VR_LOG_ERROR("camera probe: could not hook HitmanDlc.dlc rva 0x%06lX at %p",
                        kClipRva, target);
        return false;
    }
    H47VR_LOG_INFO("hooked Clip HitmanDlc.dlc rva 0x%06lX at %p", kClipRva, target);

    h47vr_update_args_target = reinterpret_cast<void*>(&ObserveCameraUpdate);
    void* update = reinterpret_cast<char*>(gameplay) + kCameraUpdateRva;
    if (!HookAddress(update, reinterpret_cast<void*>(&h47vr_detour_camera_update),
                     &h47vr_update_trampoline)) {
        H47VR_LOG_ERROR("camera probe: could not hook CamUpdate rva 0x%06lX at %p",
                        kCameraUpdateRva, update);
        return true;  // the other hook is up; not fatal
    }
    H47VR_LOG_INFO("hooked CamUpdate HitmanDlc.dlc rva 0x%06lX at %p",
                   kCameraUpdateRva, update);
    return true;
}

bool CameraProbeRearm() {
    return CameraProbeInstall();
}

// Once per presented frame, from the presentation hook: the end of a frame, so a
// write here lands before the next frame is built.
void CameraProbeFrame(unsigned long frame) {
    RefreshCameraWrite();
    if (!g_write_from_frustum.load()) {
        ApplyCameraWrite();
    }

    // Track, per frame, which dwords of the identified camera change. The
    // engine's own camera moves here on its own (the level opening sweeps it),
    // which is what makes this work without anyone touching the input devices.
    if (g_gate.empty() ||
        ::GetFileAttributesW(g_gate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    for (int i = 0; i < g_candidate_count; ++i) {
        Candidate& c = g_candidates[i];
        if (!c.tripleChanged || !Readable(c.object, kScanBytes)) {
            continue;
        }
        const auto* now = static_cast<const unsigned char*>(c.object);
        if (!c.prevValid) {
            std::memcpy(c.prev, now, kScanBytes);
            c.prevValid = true;
            continue;
        }
        for (int offset = 0; offset + 4 <= kScanBytes; offset += 4) {
            if (std::memcmp(c.prev + offset, now + offset, 4) == 0) {
                continue;
            }
            std::memcpy(c.prev + offset, now + offset, 4);
            if (c.changes[offset / 4] < 0xFFFF) {
                ++c.changes[offset / 4];
            }
        }
    }
    (void)frame;
}

// From the proxy's glFrustum detour, inside the frame being built: the camera is
// already final for this frame, and the world has not been transformed yet.
void CameraProbeOnFrustum() {
    if (g_write_from_frustum.load()) {
        ApplyCameraWrite();
    }
}

void DumpCamera(void* camera, const char* why) {
    if (camera == nullptr) {
        H47VR_LOG_INFO("camera object: none seen yet");
        return;
    }
    H47VR_LOG_INFO("object %p (%s):", camera, why);
    const auto* words = reinterpret_cast<const unsigned int*>(camera);
    H47VR_LOG_INFO("  raw   %08X %08X %08X %08X %08X %08X %08X %08X",
                   words[0], words[1], words[2], words[3],
                   words[4], words[5], words[6], words[7]);
    const auto* floats = reinterpret_cast<const float*>(camera);
    H47VR_LOG_INFO("  float %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f %10.3f",
                   floats[0], floats[1], floats[2], floats[3],
                   floats[4], floats[5], floats[6], floats[7]);
}

// While <game>\h47vr-camobj.txt exists: re-read every candidate and report the
// dwords that moved since the previous tick. Objects that never move are scenery;
// the one whose transform keeps moving is the camera.
void CameraProbeReport(unsigned long frame) {
    if (::GetFileAttributesW(g_gate.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    H47VR_LOG_INFO("frame %lu world probe: %d candidate(s), %lu cam updates, %lu clip calls",
                   frame, g_candidate_count, g_update_calls.load(), g_clip_calls.load());
    for (int i = 0; i < g_candidate_count; ++i) {
        Candidate& c = g_candidates[i];
        // Repeated, not once: a fresh object is all zeros at its first sighting
        // and only grows its transform once the engine has filled it in. The
        // AlreadyReported set keeps this from printing the same find twice.
        ScanForRotations(c.object);
        ReportBasis(c);
        ReportTriple(c);
        // What the engine itself drives on the camera, by offset: the fields it
        // rewrites most often are the position and the orientation.
        if (c.tripleChanged) {
            for (int top = 0; top < 10; ++top) {
                int best = -1;
                unsigned short bestCount = 0;
                for (int slot = 0; slot < kScanBytes / 4; ++slot) {
                    if (c.changes[slot] > bestCount) {
                        bestCount = c.changes[slot];
                        best = slot;
                    }
                }
                if (best < 0 || bestCount < 2) {
                    break;
                }
                const auto* f = reinterpret_cast<const float*>(
                    static_cast<const unsigned char*>(c.object) + best * 4);
                H47VR_LOG_INFO("  %p field +0x%03X changed %u times, now %.4f",
                               c.object, best * 4, bestCount, f[0]);
                c.changes[best] = 0;
            }
            std::memset(c.changes, 0, sizeof(c.changes));
        }
        const int changed = DiffCandidate(c);
        if (changed == 0) {
            continue;
        }
        H47VR_LOG_INFO("  %p moved %d dword(s)", c.object, changed);
    }
}

}  // namespace h47vr
