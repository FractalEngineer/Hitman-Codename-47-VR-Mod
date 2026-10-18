#include "h47vr/gl_probe.hpp"

#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"
#include "proxy_slots.h"

#include <windows.h>

#include <intrin.h>

#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

// The proxy's own tables. Every export thunk in the generated translation unit
// reads h47vr_proxy_slots, so writing a slot redirects every call to that
// function - no code is patched anywhere, and the value the slot held before is
// the real entry point to forward to.
//
// This only lives in the proxy target: h47vr_core is shared with the injected
// module and the launcher, which have no such tables to redirect.

namespace h47vr {
namespace {

// GL 1.1 fixed-function constants, spelled out so the probe needs no GL header.
constexpr unsigned int kGlModelview = 0x1700;
constexpr unsigned int kGlProjection = 0x1701;
constexpr unsigned int kGlTexture = 0x1702;

enum Count {
    kFrustum = 0,
    kOrtho,
    kLoadMatrix,
    kMultMatrix,
    kLoadIdentity,
    kPushMatrix,
    kPopMatrix,
    kMatrixMode,
    kViewport,
    kRotatef,
    kTranslatef,
    kScalef,
    kRotated,
    kTranslated,
    kLoadMatrixd,
    kMultMatrixd,
    kBegin,
    kEnd,
    kVertex3fv,
    kVertex3f,
    kVertex3d,
    kVertex2f,
    kVertexPointer,
    kDrawElements,
    kDrawArrays,
    kCountTotal,
};

const char* const kCountNames[] = {
    "frustum", "ortho", "loadMatrix", "multMatrix", "loadIdentity",
    "push", "pop", "matrixMode", "viewport",
    "rotatef", "translatef", "scalef", "rotated", "translated",
    "loadMatrixd", "multMatrixd",
    "begin", "end", "vertex3fv", "vertex3f", "vertex3d", "vertex2f",
    "vertexPointer", "drawElements", "drawArrays",
};
static_assert(kCountTotal == sizeof(kCountNames) / sizeof(kCountNames[0]),
              "counter table and name table must stay in step");

std::atomic<unsigned long> g_counts[kCountTotal];

// The current matrix target, tracked from glMatrixMode, so a logged matrix can
// say whether it landed in the projection or the modelview stack. That is the
// whole question: the modelview is where the camera lives.
std::atomic<unsigned int> g_matrix_mode{static_cast<unsigned int>(-1)};

// Log budget. A matrix is rewritten every frame, so it is logged when its
// values actually change rather than on every call. Each matrix is one line:
// the camera is found by watching the modelview rows move, and a four-line
// block per matrix would bury that in the log.
// 200 one-line matrices each: enough to watch a whole cutscene sweep the
// camera, and still small enough to read.
constexpr int kMaxMatrixLogsPerFunction = 200;
float g_last_logged[2][16] = {};
int g_matrix_logs[2] = {0, 0};

// The camera is not set with glLoadMatrixf: the modelview is built from
// glLoadIdentity followed by a short run of glRotate/glTranslate, which is the
// classic look-at. Such a call is indistinguishable from a per-object transform
// in general, so only the run immediately after an identity in modelview is
// logged - that run is the camera and nothing else.
constexpr int kCameraSequenceCalls = 5;
constexpr int kMaxCameraLogs = 24;
int g_camera_sequence = 1000;
int g_camera_logs = 0;

// Counts by target, because the camera question is entirely "does anything load
// a matrix into the modelview", and a bare total cannot answer it.
std::atomic<unsigned long> g_load_modelview{0};
std::atomic<unsigned long> g_load_other{0};

// Matrix logging is off until <game>\h47vr-glmatrix.txt appears, and stops when
// it goes away. The front end rewrites its projection and texture matrices every
// frame, so an unconditional log spends its whole budget on the menu and is
// blind by the time a level is loaded. The file is the moment to look.
std::wstring g_gate_path;
std::atomic<bool> g_gate_open{false};
unsigned int g_gate_ticks = 0;

// --- camera injection -------------------------------------------------------
//
// The engine transforms its own geometry: the modelview is always identity and
// the projection is a plain perspective, so every world vertex arrives at
// glVertex3fv already in eye space. That makes the vertex call the place an
// injected view goes: rotate the vertices and the view rotates, with no engine
// address required and no engine memory touched.
//
// The transform only runs while the current projection is the perspective one.
// The front end, the HUD and the menus all draw through an orthographic
// projection, and transforming those would scatter the interface across the
// screen.
std::wstring g_camera_path;
std::atomic<bool> g_perspective{false};
std::atomic<bool> g_camera_active{false};
std::atomic<unsigned long> g_transformed_vertices{0};
// Row-major on purpose. The first version stored this in GL's column-major
// order while the consumer applied it row by row, which is a rotation whose yaw
// is right and whose pitch mixes into the wrong axis - and quietly stays a
// rotation, so it never looks obviously broken. Row-major storage with an
// explicit multiply removes the trap.
float g_camera_matrix[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
float g_camera_yaw = 0.0f;
float g_camera_pitch = 0.0f;
unsigned int g_camera_ticks = 0;

// Rotation applied to eye-space vertices: a yaw about Y, then a pitch about the
// already-yawed X axis. Because the vertices are the scene, the head turning one
// way is the scene turning the other, so this is the inverse of the head pose -
// which is exactly what a view matrix is.
void ShapeCameraMatrix(float yawDegrees, float pitchDegrees) {
    const float yaw = yawDegrees * 3.14159265358979f / 180.0f;
    const float pitch = pitchDegrees * 3.14159265358979f / 180.0f;
    const float cy = std::cos(yaw), sy = std::sin(yaw);
    const float cp = std::cos(pitch), sp = std::sin(pitch);

    // yaw about Y, pitch about X, both in the scene's frame.
    const float yawM[3][3] = {{cy, 0.0f, sy}, {0.0f, 1.0f, 0.0f}, {-sy, 0.0f, cy}};
    const float pitchM[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, cp, -sp}, {0.0f, sp, cp}};

    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            float sum = 0.0f;
            for (int k = 0; k < 3; ++k) {
                sum += pitchM[row][k] * yawM[k][column];
            }
            g_camera_matrix[row][column] = sum;
        }
    }
}

// "yaw=<degrees> pitch=<degrees>" on one line. Any parse failure leaves the
// camera off rather than applying a half-read transform.
void RefreshCamera() {
    const bool present = ::GetFileAttributesW(g_camera_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!present) {
        if (g_camera_active.exchange(false)) {
            H47VR_LOG_INFO("gl camera: off (control file removed), %lu vertices transformed",
                           g_transformed_vertices.load());
        }
        return;
    }

    FILE* file = nullptr;
    if (::_wfopen_s(&file, g_camera_path.c_str(), L"rb") != 0 || file == nullptr) {
        return;
    }
    char text[128] = {};
    const size_t read = std::fread(text, 1, sizeof(text) - 1, file);
    std::fclose(file);
    text[read] = '\0';

    float yaw = 0.0f;
    float pitch = 0.0f;
    const char* yawAt = std::strstr(text, "yaw=");
    const char* pitchAt = std::strstr(text, "pitch=");
    if (yawAt == nullptr && pitchAt == nullptr) {
        return;
    }
    if (yawAt != nullptr) {
        yaw = static_cast<float>(std::atof(yawAt + 4));
    }
    if (pitchAt != nullptr) {
        pitch = static_cast<float>(std::atof(pitchAt + 6));
    }
    const bool wasActive = g_camera_active.exchange(true);
    if (!wasActive || yaw != g_camera_yaw || pitch != g_camera_pitch) {
        g_camera_yaw = yaw;
        g_camera_pitch = pitch;
        ShapeCameraMatrix(yaw, pitch);
        H47VR_LOG_INFO("gl camera: on (yaw=%.2f pitch=%.2f)", yaw, pitch);
        H47VR_LOG_INFO("  scene rotation | %7.4f %7.4f %7.4f | %7.4f %7.4f %7.4f "
                       "| %7.4f %7.4f %7.4f",
                       g_camera_matrix[0][0], g_camera_matrix[0][1], g_camera_matrix[0][2],
                       g_camera_matrix[1][0], g_camera_matrix[1][1], g_camera_matrix[1][2],
                       g_camera_matrix[2][0], g_camera_matrix[2][1], g_camera_matrix[2][2]);
    }
}

bool CameraTransform(float x, float y, float z, float out[3]) {
    // One file stat per 256 vertices: often enough to react within a frame or
    // two, rare enough to disappear into the per-frame cost.
    if ((++g_camera_ticks & 0xFF) == 0) {
        RefreshCamera();
    }
    if (!g_camera_active.load() || !g_perspective.load()) {
        return false;
    }
    out[0] = g_camera_matrix[0][0] * x + g_camera_matrix[0][1] * y + g_camera_matrix[0][2] * z;
    out[1] = g_camera_matrix[1][0] * x + g_camera_matrix[1][1] * y + g_camera_matrix[1][2] * z;
    out[2] = g_camera_matrix[2][0] * x + g_camera_matrix[2][1] * y + g_camera_matrix[2][2] * z;
    ++g_transformed_vertices;
    return true;
}

bool MatrixLoggingEnabled() {
    // One stat per 128 matrix calls: often enough that the gate reacts within a
    // couple of frames, rare enough that it never shows up in a profile.
    if ((++g_gate_ticks & 0x7F) != 0) {
        return g_gate_open.load();
    }
    const bool open = ::GetFileAttributesW(g_gate_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (open != g_gate_open.load()) {
        g_gate_open = open;
        H47VR_LOG_INFO("gl probe: matrix logging %s", open ? "started" : "stopped");
    }
    return open;
}

const char* ModeName(unsigned int mode) {
    switch (mode) {
        case kGlModelview: return "MODELVIEW";
        case kGlProjection: return "PROJECTION";
        case kGlTexture: return "TEXTURE";
        default: return "?";
    }
}

void LogMatrix(const char* fn, int which, const float* m) {
    if (g_matrix_logs[which] >= kMaxMatrixLogsPerFunction) {
        return;
    }
    if (!MatrixLoggingEnabled()) {
        return;
    }

    bool changed = false;
    for (int i = 0; i < 16; ++i) {
        if (std::fabs(m[i] - g_last_logged[which][i]) > 1e-4f) {
            changed = true;
            break;
        }
    }
    if (!changed) {
        return;
    }
    for (int i = 0; i < 16; ++i) {
        g_last_logged[which][i] = m[i];
    }
    ++g_matrix_logs[which];

    H47VR_LOG_INFO(
        "%s #%d %s | %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f "
        "| %8.3f %8.3f %8.3f %8.3f | %8.3f %8.3f %8.3f %8.3f",
        fn, g_matrix_logs[which], ModeName(g_matrix_mode.load()),
        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7],
        m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}

// One line per camera transform, for the first few calls after each identity.
void LogCameraCall(const char* fn, const char* format, ...) {
    if (g_camera_sequence >= kCameraSequenceCalls || g_camera_logs >= kMaxCameraLogs) {
        return;
    }
    ++g_camera_sequence;
    ++g_camera_logs;

    char detail[128];
    va_list args;
    va_start(args, format);
    ::_vsnprintf_s(detail, sizeof(detail), _TRUNCATE, format, args);
    va_end(args);
    H47VR_LOG_INFO("camera seq %d: %s %s", g_camera_sequence, fn, detail);
}

// --- detours ---------------------------------------------------------------
//
// The signatures are the Win32 GL ones: GLAPI is __stdcall on x86, so a
// mismatch here would corrupt the stack on the way out.
using FrustumFn = void(WINAPI*)(double, double, double, double, double, double);
using LoadMatrixfFn = void(WINAPI*)(const float*);
using LoadMatrixdFn = void(WINAPI*)(const double*);
using MatrixModeFn = void(WINAPI*)(unsigned int);
using ViewportFn = void(WINAPI*)(int, int, int, int);
using VoidFn = void(WINAPI*)();
using RotatefFn = void(WINAPI*)(float, float, float, float);
using TranslatefFn = void(WINAPI*)(float, float, float);
using ScalefFn = void(WINAPI*)(float, float, float);
using RotatedFn = void(WINAPI*)(double, double, double, double);
using TranslatedFn = void(WINAPI*)(double, double, double);
using BeginFn = void(WINAPI*)(unsigned int);
using Vertex3fvFn = void(WINAPI*)(const float*);
using Vertex3fFn = void(WINAPI*)(float, float, float);
using Vertex3dFn = void(WINAPI*)(double, double, double);
using Vertex2fFn = void(WINAPI*)(float, float);
using VertexPointerFn = void(WINAPI*)(int, unsigned int, unsigned int, const void*);
using DrawElementsFn = void(WINAPI*)(unsigned int, int, unsigned int, const void*);
using DrawArraysFn = void(WINAPI*)(unsigned int, int, int);

FrustumFn g_real_frustum = nullptr;
FrustumFn g_real_ortho = nullptr;
LoadMatrixfFn g_real_load_matrix = nullptr;
LoadMatrixfFn g_real_mult_matrix = nullptr;
LoadMatrixdFn g_real_load_matrix_d = nullptr;
LoadMatrixdFn g_real_mult_matrix_d = nullptr;
MatrixModeFn g_real_matrix_mode = nullptr;
ViewportFn g_real_viewport = nullptr;
VoidFn g_real_load_identity = nullptr;
VoidFn g_real_push_matrix = nullptr;
VoidFn g_real_pop_matrix = nullptr;
RotatefFn g_real_rotatef = nullptr;
TranslatefFn g_real_translatef = nullptr;
ScalefFn g_real_scalef = nullptr;
RotatedFn g_real_rotated = nullptr;
TranslatedFn g_real_translated = nullptr;
BeginFn g_real_begin = nullptr;
VoidFn g_real_end = nullptr;
Vertex3fvFn g_real_vertex3fv = nullptr;
Vertex3fFn g_real_vertex3f = nullptr;
Vertex3dFn g_real_vertex3d = nullptr;
Vertex2fFn g_real_vertex2f = nullptr;
VertexPointerFn g_real_vertex_pointer = nullptr;
DrawElementsFn g_real_draw_elements = nullptr;
DrawArraysFn g_real_draw_arrays = nullptr;

int g_arg_logs = 0;
bool g_frustum_reported = false;

void WINAPI DetourFrustum(double left, double right, double bottom, double top,
                          double zNear, double zFar) {
    ++g_counts[kFrustum];
    // glFrustum always builds a perspective, so this is a 3D pass.
    g_perspective = true;
    if (g_arg_logs < 8) {
        ++g_arg_logs;
        H47VR_LOG_INFO("glFrustum #%d l=%.4f r=%.4f b=%.4f t=%.4f near=%.4f far=%.4f",
                       g_arg_logs, left, right, bottom, top, zNear, zFar);
        if (!g_frustum_reported) {
            g_frustum_reported = true;
            const double fovY = 2.0 * std::atan(top / zNear) * 57.29577951308232;
            H47VR_LOG_INFO("  derived vertical fov %.2f degrees, aspect %.4f",
                           fovY, (right - left) / (top - bottom));
        }
    }
    g_real_frustum(left, right, bottom, top, zNear, zFar);
}

void WINAPI DetourOrtho(double left, double right, double bottom, double top,
                        double zNear, double zFar) {
    ++g_counts[kOrtho];
    g_perspective = false;
    g_real_ortho(left, right, bottom, top, zNear, zFar);
}

void WINAPI DetourLoadMatrixf(const float* matrix) {
    ++g_counts[kLoadMatrix];
    const unsigned int mode = g_matrix_mode.load();
    if (mode == kGlModelview) {
        ++g_load_modelview;
    } else {
        ++g_load_other;
        // The engine also builds its world projection by hand and loads it as a
        // 4x4. Its shape is what separates the 3D pass from the 2D ones: a
        // perspective has -1 in the fourth row and 0 in the last element, an
        // orthographic has 1 there.
        if (mode == kGlProjection && matrix != nullptr) {
            const bool perspective = std::fabs(matrix[11] + 1.0f) < 0.01f &&
                                     std::fabs(matrix[15]) < 0.01f;
            g_perspective = perspective;
        }
    }
    if (matrix != nullptr) {
        LogMatrix("glLoadMatrixf", 0, matrix);
    }
    g_real_load_matrix(matrix);
}

void WINAPI DetourMultMatrixf(const float* matrix) {
    ++g_counts[kMultMatrix];
    if (matrix != nullptr) {
        LogMatrix("glMultMatrixf", 1, matrix);
    }
    g_real_mult_matrix(matrix);
}

void WINAPI DetourLoadMatrixd(const double* matrix) {
    ++g_counts[kLoadMatrixd];
    g_real_load_matrix_d(matrix);
}

void WINAPI DetourMultMatrixd(const double* matrix) {
    ++g_counts[kMultMatrixd];
    g_real_mult_matrix_d(matrix);
}

void WINAPI DetourMatrixMode(unsigned int mode) {
    ++g_counts[kMatrixMode];
    g_matrix_mode = mode;
    g_real_matrix_mode(mode);
}

void WINAPI DetourViewport(int x, int y, int width, int height) {
    ++g_counts[kViewport];
    g_real_viewport(x, y, width, height);
}

void WINAPI DetourLoadIdentity() {
    ++g_counts[kLoadIdentity];
    // An identity in modelview starts the camera's look-at run.
    g_camera_sequence = (g_matrix_mode.load() == kGlModelview) ? 0 : 1000;
    g_real_load_identity();
}

void WINAPI DetourPushMatrix() {
    ++g_counts[kPushMatrix];
    g_real_push_matrix();
}

void WINAPI DetourPopMatrix() {
    ++g_counts[kPopMatrix];
    g_real_pop_matrix();
}

void WINAPI DetourRotatef(float angle, float x, float y, float z) {
    ++g_counts[kRotatef];
    LogCameraCall("glRotatef", "angle=%.3f axis=%.3f,%.3f,%.3f", angle, x, y, z);
    g_real_rotatef(angle, x, y, z);
}

void WINAPI DetourTranslatef(float x, float y, float z) {
    ++g_counts[kTranslatef];
    LogCameraCall("glTranslatef", "%.3f, %.3f, %.3f", x, y, z);
    g_real_translatef(x, y, z);
}

void WINAPI DetourScalef(float x, float y, float z) {
    ++g_counts[kScalef];
    LogCameraCall("glScalef", "%.3f, %.3f, %.3f", x, y, z);
    g_real_scalef(x, y, z);
}

void WINAPI DetourRotated(double angle, double x, double y, double z) {
    ++g_counts[kRotated];
    LogCameraCall("glRotated", "angle=%.3f axis=%.3f,%.3f,%.3f", angle, x, y, z);
    g_real_rotated(angle, x, y, z);
}

void WINAPI DetourTranslated(double x, double y, double z) {
    ++g_counts[kTranslated];
    LogCameraCall("glTranslated", "%.3f, %.3f, %.3f", x, y, z);
    g_real_translated(x, y, z);
}

// The vertex path is where the engine's own transform lands. If it submits
// eye-space coordinates here with only the projection left to the GL pipeline,
// then that is the only place an injected view can go.
int g_vertex_samples = 0;

// Rotating vertices at the proxy works, but it cannot be the whole answer: the
// engine has already culled against the frustum of the camera it thinks it has,
// so a large injected rotation shows a scene with holes in it. Driving the
// engine's own camera is the real fix, and its transform function is the frame
// that handed us these vertices. A stack scan at the vertex call finds it, in
// exactly the way the level-start function was found.
std::wstring g_stack_path;
unsigned int g_stack_ticks = 0;
int g_stack_samples = 0;

bool StackSamplingEnabled() {
    if (g_stack_samples >= 4) {
        return false;
    }
    if ((++g_stack_ticks & 0x3FF) != 0) {
        return false;
    }
    return ::GetFileAttributesW(g_stack_path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void DumpStack(const char* why) {
    ++g_stack_samples;
    // The return address of this call sits in the engine, and the frames above
    // it are the engine's own. Reading upward through the stack is safe: it is
    // this thread's stack and the window is small.
    void** frame = reinterpret_cast<void**>(_AddressOfReturnAddress());
    H47VR_LOG_INFO("stack sample %d (%s):", g_stack_samples, why);

    int printed = 0;
    for (int i = 0; i < 220 && printed < 40; ++i) {
        const void* value = frame[i];
        if (value == nullptr) {
            continue;
        }
        unsigned long long offset = 0;
        if (ModuleOffset(value, L"HitmanDlc.dlc", offset)) {
            H47VR_LOG_INFO("   [%3d] %p  HitmanDlc.dlc +0x%06llX", i, value, offset);
            ++printed;
        } else if (ModuleOffset(value, L"RenderOpenGL.dll", offset)) {
            H47VR_LOG_INFO("   [%3d] %p  RenderOpenGL.dll +0x%06llX", i, value, offset);
            ++printed;
        }
    }
}

void WINAPI DetourBegin(unsigned int mode) {
    ++g_counts[kBegin];
    g_real_begin(mode);
}

void WINAPI DetourEnd() {
    ++g_counts[kEnd];
    g_real_end();
}

void WINAPI DetourVertex3fv(const float* vertex) {
    const unsigned long count = ++g_counts[kVertex3fv];
    if (vertex != nullptr && (count <= 3 || count % 200000 == 0)) {
        H47VR_LOG_INFO("glVertex3fv #%lu %.3f, %.3f, %.3f",
                       count, vertex[0], vertex[1], vertex[2]);
    }
    if (StackSamplingEnabled()) {
        DumpStack("glVertex3fv");
    }
    float transformed[3];
    if (vertex != nullptr &&
        CameraTransform(vertex[0], vertex[1], vertex[2], transformed)) {
        g_real_vertex3fv(transformed);
        return;
    }
    g_real_vertex3fv(vertex);
}

void WINAPI DetourVertex3f(float x, float y, float z) {
    ++g_counts[kVertex3f];
    float transformed[3];
    if (CameraTransform(x, y, z, transformed)) {
        g_real_vertex3f(transformed[0], transformed[1], transformed[2]);
        return;
    }
    g_real_vertex3f(x, y, z);
}

void WINAPI DetourVertex3d(double x, double y, double z) {
    ++g_counts[kVertex3d];
    g_real_vertex3d(x, y, z);
}

void WINAPI DetourVertex2f(float x, float y) {
    ++g_counts[kVertex2f];
    g_real_vertex2f(x, y);
}

void WINAPI DetourVertexPointer(int size, unsigned int type, unsigned int stride,
                                const void* pointer) {
    ++g_counts[kVertexPointer];
    g_real_vertex_pointer(size, type, stride, pointer);
}

void WINAPI DetourDrawElements(unsigned int mode, int count, unsigned int type,
                               const void* indices) {
    ++g_counts[kDrawElements];
    g_real_draw_elements(mode, count, type, indices);
}

void WINAPI DetourDrawArrays(unsigned int mode, int first, int count) {
    ++g_counts[kDrawArrays];
    g_real_draw_arrays(mode, first, count);
}

// --- installation ----------------------------------------------------------

bool SameName(const char* a, const char* b) {
    if (a == nullptr || b == nullptr) {
        return false;
    }
    while (*a != '\0' && *a == *b) {
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

int ProxyIndex(const char* name) {
    for (int i = 0; i < h47vr_proxy_count; ++i) {
        if (SameName(h47vr_proxy_names[i], name)) {
            return i;
        }
    }
    return -1;
}

struct Intercept {
    const char* name;
    void* detour;
    void** real;
};

// Comma-separated name list, "all", or "none". Kept deliberately dumb: this
// runs once, on the mod's own start-up thread.
bool Selected(const char* filter, const char* name) {
    if (filter == nullptr || filter[0] == '\0' || SameName(filter, "all")) {
        return true;
    }
    if (SameName(filter, "none")) {
        return false;
    }
    const char* cursor = filter;
    while (*cursor != '\0') {
        const char* end = cursor;
        while (*end != '\0' && *end != ',') {
            ++end;
        }
        const char* a = cursor;
        const char* b = name;
        bool match = true;
        while (a < end) {
            if (b == nullptr || *a != *b) {
                match = false;
                break;
            }
            ++a;
            ++b;
        }
        if (match && b != nullptr && *b == '\0') {
            return true;
        }
        cursor = (*end == ',') ? end + 1 : end;
    }
    return false;
}

bool InstallImpl(const char* filter) {
    // Resolve every slot through the proxy's own resolver first: the redirect
    // below replaces a slot, so the real entry point has to be captured before
    // that happens.
    h47vr_proxy_resolve();

    g_gate_path = JoinPath(mod::GameDirectory(), L"h47vr-glmatrix.txt");
    g_camera_path = JoinPath(mod::GameDirectory(), L"h47vr-cam.txt");
    g_stack_path = JoinPath(mod::GameDirectory(), L"h47vr-stack.txt");

    const Intercept intercepts[] = {
        {"glFrustum", reinterpret_cast<void*>(&DetourFrustum),
         reinterpret_cast<void**>(&g_real_frustum)},
        {"glOrtho", reinterpret_cast<void*>(&DetourOrtho),
         reinterpret_cast<void**>(&g_real_ortho)},
        {"glLoadMatrixf", reinterpret_cast<void*>(&DetourLoadMatrixf),
         reinterpret_cast<void**>(&g_real_load_matrix)},
        {"glMultMatrixf", reinterpret_cast<void*>(&DetourMultMatrixf),
         reinterpret_cast<void**>(&g_real_mult_matrix)},
        {"glLoadMatrixd", reinterpret_cast<void*>(&DetourLoadMatrixd),
         reinterpret_cast<void**>(&g_real_load_matrix_d)},
        {"glMultMatrixd", reinterpret_cast<void*>(&DetourMultMatrixd),
         reinterpret_cast<void**>(&g_real_mult_matrix_d)},
        {"glMatrixMode", reinterpret_cast<void*>(&DetourMatrixMode),
         reinterpret_cast<void**>(&g_real_matrix_mode)},
        {"glViewport", reinterpret_cast<void*>(&DetourViewport),
         reinterpret_cast<void**>(&g_real_viewport)},
        {"glLoadIdentity", reinterpret_cast<void*>(&DetourLoadIdentity),
         reinterpret_cast<void**>(&g_real_load_identity)},
        {"glPushMatrix", reinterpret_cast<void*>(&DetourPushMatrix),
         reinterpret_cast<void**>(&g_real_push_matrix)},
        {"glPopMatrix", reinterpret_cast<void*>(&DetourPopMatrix),
         reinterpret_cast<void**>(&g_real_pop_matrix)},
        {"glRotatef", reinterpret_cast<void*>(&DetourRotatef),
         reinterpret_cast<void**>(&g_real_rotatef)},
        {"glTranslatef", reinterpret_cast<void*>(&DetourTranslatef),
         reinterpret_cast<void**>(&g_real_translatef)},
        {"glScalef", reinterpret_cast<void*>(&DetourScalef),
         reinterpret_cast<void**>(&g_real_scalef)},
        {"glRotated", reinterpret_cast<void*>(&DetourRotated),
         reinterpret_cast<void**>(&g_real_rotated)},
        {"glTranslated", reinterpret_cast<void*>(&DetourTranslated),
         reinterpret_cast<void**>(&g_real_translated)},
        {"glBegin", reinterpret_cast<void*>(&DetourBegin),
         reinterpret_cast<void**>(&g_real_begin)},
        {"glEnd", reinterpret_cast<void*>(&DetourEnd),
         reinterpret_cast<void**>(&g_real_end)},
        {"glVertex3fv", reinterpret_cast<void*>(&DetourVertex3fv),
         reinterpret_cast<void**>(&g_real_vertex3fv)},
        {"glVertex3f", reinterpret_cast<void*>(&DetourVertex3f),
         reinterpret_cast<void**>(&g_real_vertex3f)},
        {"glVertex3d", reinterpret_cast<void*>(&DetourVertex3d),
         reinterpret_cast<void**>(&g_real_vertex3d)},
        {"glVertex2f", reinterpret_cast<void*>(&DetourVertex2f),
         reinterpret_cast<void**>(&g_real_vertex2f)},
        {"glVertexPointer", reinterpret_cast<void*>(&DetourVertexPointer),
         reinterpret_cast<void**>(&g_real_vertex_pointer)},
        {"glDrawElements", reinterpret_cast<void*>(&DetourDrawElements),
         reinterpret_cast<void**>(&g_real_draw_elements)},
        {"glDrawArrays", reinterpret_cast<void*>(&DetourDrawArrays),
         reinterpret_cast<void**>(&g_real_draw_arrays)},
    };

    const int wanted = static_cast<int>(sizeof(intercepts) / sizeof(intercepts[0]));
    int installed = 0;
    for (const Intercept& intercept : intercepts) {
        const int index = ProxyIndex(intercept.name);
        if (index < 0) {
            H47VR_LOG_WARN("gl probe: the proxy has no export '%s'", intercept.name);
            continue;
        }
        if (!Selected(filter, intercept.name)) {
            continue;
        }
        void* slot = h47vr_proxy_slots[index];
        if (slot == nullptr) {
            H47VR_LOG_WARN("gl probe: slot for '%s' did not resolve", intercept.name);
            continue;
        }
        if (slot == intercept.detour) {
            // Already redirected: the slot table survives a second install, so
            // the real entry point must not be overwritten with the detour.
            ++installed;
            continue;
        }
        *intercept.real = slot;
        h47vr_proxy_slots[index] = intercept.detour;
        ++installed;
    }

    H47VR_LOG_INFO("gl probe: %d of %d pipeline entries intercepted (filter '%s')",
                   installed, wanted, filter != nullptr ? filter : "all");
    return installed > 0;
}

void ReportImpl(unsigned long frame) {
    char line[600];
    int used = 0;
    for (int i = 0; i < kCountTotal; ++i) {
        if (used >= static_cast<int>(sizeof(line)) - 32) {
            break;
        }
        const int written = ::_snprintf_s(line + used, sizeof(line) - used, _TRUNCATE,
                                          "%s%s=%lu", i == 0 ? "" : " ",
                                          kCountNames[i], g_counts[i].load());
        if (written <= 0) {
            break;
        }
        used += written;
    }
    H47VR_LOG_INFO("frame %lu gl calls: %s", frame, line);
    H47VR_LOG_INFO("frame %lu modelview loads: %lu (other targets %lu)",
                   frame, g_load_modelview.load(), g_load_other.load());
    H47VR_LOG_INFO("frame %lu camera injection: active=%s perspective=%s transformed=%lu",
                   frame, g_camera_active.load() ? "yes" : "no",
                   g_perspective.load() ? "yes" : "no",
                   g_transformed_vertices.load());
}

}  // namespace

// Called by the proxy's DllMain: the pointer indirection is what keeps
// h47vr_core free of a link dependency on this module.
void GlProbePublish() {
    g_gl_probe_install = &InstallImpl;
    g_gl_probe_report = &ReportImpl;
}

}  // namespace h47vr
