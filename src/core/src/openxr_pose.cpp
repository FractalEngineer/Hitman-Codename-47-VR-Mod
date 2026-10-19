#include "h47vr/openxr_pose.hpp"

#include "h47vr/gl_probe.hpp"
#include "h47vr/log.hpp"
#include "h47vr/look_driver.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/stereo_renderer.hpp"
#include "h47vr/symbols.hpp"

#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_GRAPHICS_API_OPENGL 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <GL/gl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <mutex>
#include <vector>

namespace h47vr {
namespace {

const char* kOpenGlExtension = "XR_KHR_opengl_enable";

std::wstring g_gate_path;
std::wstring g_runtime_path;
bool g_runtime_override_set = false;
bool g_had_previous_runtime = false;
std::wstring g_previous_runtime;
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_session_running{false};
std::mutex g_pose_mutex;
OpenXrEyePose g_pose;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local_space = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;

// Minimal OpenGL entry points for attaching XR swapchain textures to FBOs and
// scaling the engine's completed backbuffer into them. The engine itself is GL
// 1.1, so these come from the driver through wglGetProcAddress.
constexpr GLenum kGlFramebuffer = 0x8D40;
constexpr GLenum kGlRenderbuffer = 0x8D41;
constexpr GLenum kGlReadFramebuffer = 0x8CA8;
constexpr GLenum kGlDrawFramebuffer = 0x8CA9;
constexpr GLenum kGlReadFramebufferBinding = 0x8CAA;
constexpr GLenum kGlDrawFramebufferBinding = 0x8CA6;
constexpr GLenum kGlRenderbufferBinding = 0x8CA7;
constexpr GLenum kGlColorAttachment0 = 0x8CE0;
constexpr GLenum kGlDepthAttachment = 0x8D00;
constexpr GLenum kGlFramebufferComplete = 0x8CD5;
constexpr GLenum kGlTexture2d = 0x0DE1;
constexpr GLenum kGlRgba8 = 0x8058;
constexpr GLenum kGlDepthComponent24 = 0x81A6;
constexpr GLenum kGlScissorTest = 0x0C11;
constexpr GLbitfield kGlColorBufferBit = 0x00004000;
constexpr GLbitfield kGlDepthBufferBit = 0x00000100;
constexpr GLbitfield kGlAllAttribBits = 0xFFFFFFFF;

struct EyeSwapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<XrSwapchainImageOpenGLKHR> images;
    std::vector<GLuint> framebuffers;
    std::vector<GLuint> depthRenderbuffers;
};

EyeSwapchain g_eyes[2];
GLenum g_swapchain_format = kGlRgba8;
bool g_presentation_ready = false;
int g_last_submitted_mode = -1;
uint32_t g_acquired_images[2] = {UINT32_MAX, UINT32_MAX};
XrFrameState g_render_frame{XR_TYPE_FRAME_STATE};
XrView g_render_views[2] = {
    XrView{XR_TYPE_VIEW},
    XrView{XR_TYPE_VIEW},
};
bool g_render_frame_open = false;
bool g_direct_stereo = false;
bool g_render_views_valid = false;
unsigned long g_direct_frame_count = 0;

using WglGetProcAddressFn = PROC(WINAPI*)(LPCSTR);
using GlGenFramebuffersFn = void(APIENTRY*)(GLsizei, GLuint*);
using GlBindFramebufferFn = void(APIENTRY*)(GLenum, GLuint);
using GlFramebufferTexture2DFn =
    void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint, GLint);
using GlCheckFramebufferStatusFn = GLenum(APIENTRY*)(GLenum);
using GlDeleteFramebuffersFn = void(APIENTRY*)(GLsizei, const GLuint*);
using GlBlitFramebufferFn = void(APIENTRY*)(GLint, GLint, GLint, GLint, GLint,
                                            GLint, GLint, GLint, GLbitfield,
                                            GLenum);
using GlGetIntegervFn = void(APIENTRY*)(GLenum, GLint*);
using GlGenRenderbuffersFn = void(APIENTRY*)(GLsizei, GLuint*);
using GlBindRenderbufferFn = void(APIENTRY*)(GLenum, GLuint);
using GlRenderbufferStorageFn =
    void(APIENTRY*)(GLenum, GLenum, GLsizei, GLsizei);
using GlFramebufferRenderbufferFn =
    void(APIENTRY*)(GLenum, GLenum, GLenum, GLuint);
using GlDeleteRenderbuffersFn = void(APIENTRY*)(GLsizei, const GLuint*);
using GlClearFn = void(APIENTRY*)(GLbitfield);
using GlClearColorFn = void(APIENTRY*)(GLfloat, GLfloat, GLfloat, GLfloat);
using GlColorMaskFn = void(APIENTRY*)(GLboolean, GLboolean, GLboolean,
                                      GLboolean);
using GlDepthMaskFn = void(APIENTRY*)(GLboolean);
using GlDisableFn = void(APIENTRY*)(GLenum);
using GlPushAttribFn = void(APIENTRY*)(GLbitfield);
using GlVoidFn = void(APIENTRY*)();
using GlReadPixelsFn = void(APIENTRY*)(GLint, GLint, GLsizei, GLsizei,
                                       GLenum, GLenum, void*);

GlGenFramebuffersFn g_gl_gen_framebuffers = nullptr;
GlBindFramebufferFn g_gl_bind_framebuffer = nullptr;
GlFramebufferTexture2DFn g_gl_framebuffer_texture_2d = nullptr;
GlCheckFramebufferStatusFn g_gl_check_framebuffer_status = nullptr;
GlDeleteFramebuffersFn g_gl_delete_framebuffers = nullptr;
GlBlitFramebufferFn g_gl_blit_framebuffer = nullptr;
GlGetIntegervFn g_gl_get_integer = nullptr;
GlGenRenderbuffersFn g_gl_gen_renderbuffers = nullptr;
GlBindRenderbufferFn g_gl_bind_renderbuffer = nullptr;
GlRenderbufferStorageFn g_gl_renderbuffer_storage = nullptr;
GlFramebufferRenderbufferFn g_gl_framebuffer_renderbuffer = nullptr;
GlDeleteRenderbuffersFn g_gl_delete_renderbuffers = nullptr;
GlClearFn g_gl_clear = nullptr;
GlClearColorFn g_gl_clear_color = nullptr;
GlColorMaskFn g_gl_color_mask = nullptr;
GlDepthMaskFn g_gl_depth_mask = nullptr;
GlDisableFn g_gl_disable = nullptr;
GlPushAttribFn g_gl_push_attrib = nullptr;
GlVoidFn g_gl_pop_attrib = nullptr;
GlReadPixelsFn g_gl_read_pixels = nullptr;

void ReleaseAcquiredImages();

void* GlProcedure(HMODULE openGl, WglGetProcAddressFn getProcAddress,
                  const char* name) {
    void* address = getProcAddress != nullptr
                        ? reinterpret_cast<void*>(getProcAddress(name))
                        : nullptr;
    if (address == nullptr) {
        address = reinterpret_cast<void*>(::GetProcAddress(openGl, name));
    }
    return address;
}

void InvalidatePose() {
    std::lock_guard<std::mutex> lock(g_pose_mutex);
    g_pose.valid = false;
    g_pose.fovValid = false;
}

bool Succeeded(XrResult result, const char* step) {
    if (XR_SUCCEEDED(result)) {
        H47VR_LOG_INFO("openxr: %s", step);
        return true;
    }
    H47VR_LOG_WARN("openxr: %s failed (XrResult %d)", step,
                   static_cast<int>(result));
    return false;
}

std::wstring ReadFirstWideLine(const std::wstring& path) {
    std::wifstream file(path);
    std::wstring line;
    while (std::getline(file, line)) {
        while (!line.empty() &&
               (line.back() == L'\r' || line.back() == L'\n' ||
                line.back() == L' ' || line.back() == L'\t')) {
            line.pop_back();
        }
        const auto first = line.find_first_not_of(L" \t");
        if (first == std::wstring::npos || line[first] == L'#') {
            continue;
        }
        return line.substr(first);
    }
    return {};
}

void ApplyRuntimeOverride() {
    if (g_runtime_path.empty()) {
        return;
    }
    const std::wstring requested = ReadFirstWideLine(g_runtime_path);
    if (!requested.empty()) {
        if (!g_runtime_override_set) {
            const DWORD size =
                ::GetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr, 0);
            if (size > 0) {
                g_previous_runtime.resize(size);
                const DWORD written = ::GetEnvironmentVariableW(
                    L"XR_RUNTIME_JSON", g_previous_runtime.data(), size);
                g_had_previous_runtime = written > 0;
                if (written > 0) {
                    g_previous_runtime.resize(written);
                }
            } else {
                g_had_previous_runtime = false;
                g_previous_runtime.clear();
            }
            g_runtime_override_set = true;
            ::SetEnvironmentVariableW(L"XR_RUNTIME_JSON", requested.c_str());
            H47VR_LOG_INFO("openxr: runtime override %s",
                           Narrow(requested).c_str());
        }
    } else if (g_runtime_override_set) {
        ::SetEnvironmentVariableW(
            L"XR_RUNTIME_JSON",
            g_had_previous_runtime ? g_previous_runtime.c_str() : nullptr);
        g_runtime_override_set = false;
        g_had_previous_runtime = false;
        g_previous_runtime.clear();
    }
}

bool HasExtension(const std::vector<XrExtensionProperties>& extensions,
                  const char* name) {
    for (const XrExtensionProperties& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

bool ValidFov(const XrFovf& fov) {
    constexpr float kHalfPi = 1.5707963267948966f;
    return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
           std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown) &&
           fov.angleLeft < 0.0f && fov.angleRight > 0.0f &&
           fov.angleUp > 0.0f && fov.angleDown < 0.0f &&
           fov.angleLeft > -kHalfPi && fov.angleRight < kHalfPi &&
           fov.angleUp < kHalfPi && fov.angleDown > -kHalfPi;
}

HGLRC CurrentGlContext() {
    HMODULE openGl = LoadedModule(L"opengl32.dll");
    if (openGl == nullptr) {
        return nullptr;
    }
    using GetContextFn = HGLRC(WINAPI*)();
    auto getContext = reinterpret_cast<GetContextFn>(
        ::GetProcAddress(openGl, "wglGetCurrentContext"));
    return getContext != nullptr ? getContext() : nullptr;
}

bool LoadGlPresentationFunctions() {
    HMODULE openGl = LoadedModule(L"opengl32.dll");
    if (openGl == nullptr) {
        return false;
    }
    auto getProcAddress = reinterpret_cast<WglGetProcAddressFn>(
        ::GetProcAddress(openGl, "wglGetProcAddress"));
    g_gl_gen_framebuffers = reinterpret_cast<GlGenFramebuffersFn>(
        GlProcedure(openGl, getProcAddress, "glGenFramebuffers"));
    g_gl_bind_framebuffer = reinterpret_cast<GlBindFramebufferFn>(
        GlProcedure(openGl, getProcAddress, "glBindFramebuffer"));
    g_gl_framebuffer_texture_2d = reinterpret_cast<GlFramebufferTexture2DFn>(
        GlProcedure(openGl, getProcAddress, "glFramebufferTexture2D"));
    g_gl_check_framebuffer_status = reinterpret_cast<GlCheckFramebufferStatusFn>(
        GlProcedure(openGl, getProcAddress, "glCheckFramebufferStatus"));
    g_gl_delete_framebuffers = reinterpret_cast<GlDeleteFramebuffersFn>(
        GlProcedure(openGl, getProcAddress, "glDeleteFramebuffers"));
    g_gl_blit_framebuffer = reinterpret_cast<GlBlitFramebufferFn>(
        GlProcedure(openGl, getProcAddress, "glBlitFramebuffer"));
    g_gl_get_integer = reinterpret_cast<GlGetIntegervFn>(
        GlProcedure(openGl, getProcAddress, "glGetIntegerv"));
    g_gl_gen_renderbuffers = reinterpret_cast<GlGenRenderbuffersFn>(
        GlProcedure(openGl, getProcAddress, "glGenRenderbuffers"));
    g_gl_bind_renderbuffer = reinterpret_cast<GlBindRenderbufferFn>(
        GlProcedure(openGl, getProcAddress, "glBindRenderbuffer"));
    g_gl_renderbuffer_storage = reinterpret_cast<GlRenderbufferStorageFn>(
        GlProcedure(openGl, getProcAddress, "glRenderbufferStorage"));
    g_gl_framebuffer_renderbuffer =
        reinterpret_cast<GlFramebufferRenderbufferFn>(
            GlProcedure(openGl, getProcAddress, "glFramebufferRenderbuffer"));
    g_gl_delete_renderbuffers = reinterpret_cast<GlDeleteRenderbuffersFn>(
        GlProcedure(openGl, getProcAddress, "glDeleteRenderbuffers"));
    g_gl_clear = reinterpret_cast<GlClearFn>(
        GlProcedure(openGl, getProcAddress, "glClear"));
    g_gl_clear_color = reinterpret_cast<GlClearColorFn>(
        GlProcedure(openGl, getProcAddress, "glClearColor"));
    g_gl_color_mask = reinterpret_cast<GlColorMaskFn>(
        GlProcedure(openGl, getProcAddress, "glColorMask"));
    g_gl_depth_mask = reinterpret_cast<GlDepthMaskFn>(
        GlProcedure(openGl, getProcAddress, "glDepthMask"));
    g_gl_disable = reinterpret_cast<GlDisableFn>(
        GlProcedure(openGl, getProcAddress, "glDisable"));
    g_gl_push_attrib = reinterpret_cast<GlPushAttribFn>(
        GlProcedure(openGl, getProcAddress, "glPushAttrib"));
    g_gl_pop_attrib = reinterpret_cast<GlVoidFn>(
        GlProcedure(openGl, getProcAddress, "glPopAttrib"));
    g_gl_read_pixels = reinterpret_cast<GlReadPixelsFn>(
        GlProcedure(openGl, getProcAddress, "glReadPixels"));
    return g_gl_gen_framebuffers != nullptr &&
           g_gl_bind_framebuffer != nullptr &&
           g_gl_framebuffer_texture_2d != nullptr &&
           g_gl_check_framebuffer_status != nullptr &&
           g_gl_delete_framebuffers != nullptr &&
           g_gl_blit_framebuffer != nullptr && g_gl_get_integer != nullptr &&
           g_gl_gen_renderbuffers != nullptr &&
           g_gl_bind_renderbuffer != nullptr &&
           g_gl_renderbuffer_storage != nullptr &&
           g_gl_framebuffer_renderbuffer != nullptr &&
           g_gl_delete_renderbuffers != nullptr && g_gl_clear != nullptr &&
           g_gl_clear_color != nullptr && g_gl_color_mask != nullptr &&
           g_gl_depth_mask != nullptr && g_gl_disable != nullptr &&
           g_gl_push_attrib != nullptr && g_gl_pop_attrib != nullptr &&
           g_gl_read_pixels != nullptr;
}

void DestroyEyeSwapchain(EyeSwapchain& eye) {
    if (!eye.framebuffers.empty() && g_gl_delete_framebuffers != nullptr) {
        g_gl_delete_framebuffers(
            static_cast<GLsizei>(eye.framebuffers.size()),
            eye.framebuffers.data());
    }
    eye.framebuffers.clear();
    if (!eye.depthRenderbuffers.empty() &&
        g_gl_delete_renderbuffers != nullptr) {
        g_gl_delete_renderbuffers(
            static_cast<GLsizei>(eye.depthRenderbuffers.size()),
            eye.depthRenderbuffers.data());
    }
    eye.depthRenderbuffers.clear();
    eye.images.clear();
    if (eye.handle != XR_NULL_HANDLE) {
        ::xrDestroySwapchain(eye.handle);
        eye.handle = XR_NULL_HANDLE;
    }
    eye.width = 0;
    eye.height = 0;
}

void DestroyGraphics() {
    if (g_render_frame_open && g_session != XR_NULL_HANDLE) {
        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = g_render_frame.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = 0;
        endInfo.layers = nullptr;
        ::xrEndFrame(g_session, &endInfo);
        g_render_frame_open = false;
    }
    ReleaseAcquiredImages();
    for (EyeSwapchain& eye : g_eyes) {
        DestroyEyeSwapchain(eye);
    }
    g_presentation_ready = false;
    g_last_submitted_mode = -1;
    g_render_frame_open = false;
    g_direct_stereo = false;
    g_render_views_valid = false;
}

void ShutdownXr() {
    DestroyGraphics();
    if (g_session != XR_NULL_HANDLE) {
        if (g_session_running.exchange(false)) {
            ::xrEndSession(g_session);
        }
        if (g_local_space != XR_NULL_HANDLE) {
            ::xrDestroySpace(g_local_space);
            g_local_space = XR_NULL_HANDLE;
        }
        ::xrDestroySession(g_session);
        g_session = XR_NULL_HANDLE;
    } else if (g_local_space != XR_NULL_HANDLE) {
        g_local_space = XR_NULL_HANDLE;
    }
    if (g_instance != XR_NULL_HANDLE) {
        ::xrDestroyInstance(g_instance);
        g_instance = XR_NULL_HANDLE;
    }
    g_system = XR_NULL_SYSTEM_ID;
    g_session_state = XR_SESSION_STATE_UNKNOWN;
    g_attempted = false;
    if (g_runtime_override_set) {
        ::SetEnvironmentVariableW(
            L"XR_RUNTIME_JSON",
            g_had_previous_runtime ? g_previous_runtime.c_str() : nullptr);
        g_runtime_override_set = false;
        g_had_previous_runtime = false;
        g_previous_runtime.clear();
    }
    InvalidatePose();
}

bool CreateEyeSwapchain(XrViewConfigurationView view, EyeSwapchain& eye,
                        int index) {
    XrSwapchainCreateInfo createInfo{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    createInfo.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                            XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    createInfo.format = g_swapchain_format;
    createInfo.sampleCount = 1;
    createInfo.width = view.recommendedImageRectWidth;
    createInfo.height = view.recommendedImageRectHeight;
    createInfo.faceCount = 1;
    createInfo.arraySize = 1;
    createInfo.mipCount = 1;
    if (!Succeeded(::xrCreateSwapchain(g_session, &createInfo, &eye.handle),
                   index == 0 ? "xrCreateSwapchain (left)"
                              : "xrCreateSwapchain (right)")) {
        return false;
    }
    eye.width = createInfo.width;
    eye.height = createInfo.height;

    uint32_t imageCount = 0;
    XrResult result = ::xrEnumerateSwapchainImages(eye.handle, 0, &imageCount,
                                                   nullptr);
    if (XR_FAILED(result) || imageCount == 0) {
        H47VR_LOG_WARN("openxr: xrEnumerateSwapchainImages failed (%d)",
                       static_cast<int>(result));
        return false;
    }
    eye.images.assign(
        imageCount,
        XrSwapchainImageOpenGLKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
    result = ::xrEnumerateSwapchainImages(
        eye.handle, imageCount, &imageCount,
        reinterpret_cast<XrSwapchainImageBaseHeader*>(eye.images.data()));
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: fill swapchain images failed (%d)",
                       static_cast<int>(result));
        return false;
    }
    eye.images.resize(imageCount);
    eye.framebuffers.resize(imageCount, 0);
    eye.depthRenderbuffers.resize(imageCount, 0);

    GLint previousFramebuffer = 0;
    GLint previousRenderbuffer = 0;
    g_gl_get_integer(kGlDrawFramebufferBinding, &previousFramebuffer);
    g_gl_get_integer(kGlRenderbufferBinding, &previousRenderbuffer);
    for (uint32_t i = 0; i < imageCount; ++i) {
        g_gl_gen_framebuffers(1, &eye.framebuffers[i]);
        g_gl_gen_renderbuffers(1, &eye.depthRenderbuffers[i]);
        g_gl_bind_renderbuffer(kGlRenderbuffer, eye.depthRenderbuffers[i]);
        g_gl_renderbuffer_storage(
            kGlRenderbuffer, kGlDepthComponent24,
            static_cast<GLsizei>(eye.width),
            static_cast<GLsizei>(eye.height));
        g_gl_bind_framebuffer(kGlFramebuffer, eye.framebuffers[i]);
        g_gl_framebuffer_texture_2d(kGlFramebuffer, kGlColorAttachment0,
                                    kGlTexture2d, eye.images[i].image, 0);
        g_gl_framebuffer_renderbuffer(kGlFramebuffer, kGlDepthAttachment,
                                      kGlRenderbuffer,
                                      eye.depthRenderbuffers[i]);
        if (g_gl_check_framebuffer_status(kGlFramebuffer) !=
            kGlFramebufferComplete) {
            g_gl_bind_framebuffer(kGlFramebuffer,
                                  static_cast<GLuint>(previousFramebuffer));
            g_gl_bind_renderbuffer(kGlRenderbuffer,
                                   static_cast<GLuint>(previousRenderbuffer));
            H47VR_LOG_WARN("openxr: swapchain FBO %u incomplete for eye %d", i,
                           index);
            return false;
        }
    }
    g_gl_bind_framebuffer(kGlFramebuffer,
                          static_cast<GLuint>(previousFramebuffer));
    g_gl_bind_renderbuffer(kGlRenderbuffer,
                           static_cast<GLuint>(previousRenderbuffer));
    H47VR_LOG_INFO("openxr: eye %d swapchain %ux%u, %u image(s), format "
                   "0x%04X, depth 24",
                   index, eye.width, eye.height, imageCount,
                   static_cast<unsigned int>(g_swapchain_format));
    return true;
}

bool CreatePresentationSwapchains() {
    if (!LoadGlPresentationFunctions()) {
        H47VR_LOG_WARN(
            "openxr: required framebuffer/blit GL entry points are unavailable");
        return false;
    }

    uint32_t viewCount = 0;
    XrResult result = ::xrEnumerateViewConfigurationViews(
        g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
        &viewCount, nullptr);
    if (XR_FAILED(result) || viewCount < 2) {
        H47VR_LOG_WARN("openxr: could not enumerate two stereo views (%d)",
                       static_cast<int>(result));
        return false;
    }
    std::vector<XrViewConfigurationView> views(
        viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
    result = ::xrEnumerateViewConfigurationViews(
        g_instance, g_system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
        viewCount, &viewCount, views.data());
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: fill stereo view configuration failed (%d)",
                       static_cast<int>(result));
        return false;
    }

    uint32_t formatCount = 0;
    result = ::xrEnumerateSwapchainFormats(g_session, 0, &formatCount, nullptr);
    if (XR_FAILED(result) || formatCount == 0) {
        H47VR_LOG_WARN("openxr: no swapchain formats available (%d)",
                       static_cast<int>(result));
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    result = ::xrEnumerateSwapchainFormats(g_session, formatCount, &formatCount,
                                           formats.data());
    if (XR_FAILED(result)) {
        return false;
    }
    g_swapchain_format = static_cast<GLenum>(formats.front());
    for (int64_t format : formats) {
        if (format == static_cast<int64_t>(kGlRgba8)) {
            g_swapchain_format = kGlRgba8;
            break;
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        if (!CreateEyeSwapchain(views[eye], g_eyes[eye], eye)) {
            DestroyGraphics();
            return false;
        }
    }
    g_presentation_ready = true;
    return true;
}

void PollEvents() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    while (::xrPollEvent(g_instance, &event) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* changed =
                reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
            g_session_state = changed->state;
            H47VR_LOG_INFO("openxr: session state %d",
                           static_cast<int>(g_session_state));

            if (g_session_state == XR_SESSION_STATE_READY &&
                !g_session_running.load()) {
                const XrSessionBeginInfo beginInfo{
                    XR_TYPE_SESSION_BEGIN_INFO,
                    nullptr,
                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                };
                if (Succeeded(::xrBeginSession(g_session, &beginInfo),
                              "xrBeginSession")) {
                    g_session_running = true;
                }
            } else if (g_session_state == XR_SESSION_STATE_STOPPING) {
                g_session_running = false;
                ::xrEndSession(g_session);
            }
        }
        event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

bool Initialize(HDC device) {
    HGLRC context = CurrentGlContext();
    if (context == nullptr) {
        H47VR_LOG_WARN("openxr: no current WGL context on the render thread");
        return false;
    }

    uint32_t extensionCount = 0;
    XrResult result = ::xrEnumerateInstanceExtensionProperties(
        nullptr, 0, &extensionCount, nullptr);
    if (!Succeeded(result, "xrEnumerateInstanceExtensionProperties")) {
        return false;
    }
    std::vector<XrExtensionProperties> extensions(
        extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    result = ::xrEnumerateInstanceExtensionProperties(
        nullptr, extensionCount, &extensionCount, extensions.data());
    if (!Succeeded(result, "enumerate extension details")) {
        return false;
    }
    if (!HasExtension(extensions, kOpenGlExtension)) {
        H47VR_LOG_WARN("openxr: active runtime does not advertise %s",
                       kOpenGlExtension);
        return false;
    }

    const char* enabledExtension = kOpenGlExtension;
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    createInfo.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    strcpy_s(createInfo.applicationInfo.applicationName, "h47vr");
    createInfo.applicationInfo.applicationVersion = 1;
    strcpy_s(createInfo.applicationInfo.engineName, "Hitman");
    createInfo.enabledExtensionCount = 1;
    createInfo.enabledExtensionNames = &enabledExtension;
    if (!Succeeded(::xrCreateInstance(&createInfo, &g_instance),
                   "xrCreateInstance (OpenGL)")) {
        return false;
    }

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (!Succeeded(::xrGetSystem(g_instance, &systemInfo, &g_system),
                   "xrGetSystem (head mounted display)")) {
        return false;
    }

    PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
    result = ::xrGetInstanceProcAddr(
        g_instance, "xrGetOpenGLGraphicsRequirementsKHR",
        reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
    if (XR_FAILED(result) || getRequirements == nullptr) {
        H47VR_LOG_WARN("openxr: xrGetOpenGLGraphicsRequirementsKHR unavailable");
        return false;
    }
    XrGraphicsRequirementsOpenGLKHR requirements{
        XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (!Succeeded(getRequirements(g_instance, g_system, &requirements),
                   "xrGetOpenGLGraphicsRequirementsKHR")) {
        return false;
    }
    H47VR_LOG_INFO("openxr: GL requirements %u.%u - %u.%u",
                   XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                   XR_VERSION_MINOR(requirements.minApiVersionSupported),
                   XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                   XR_VERSION_MINOR(requirements.maxApiVersionSupported));

    XrGraphicsBindingOpenGLWin32KHR binding{
        XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    binding.hDC = device;
    binding.hGLRC = context;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g_system;
    if (!Succeeded(::xrCreateSession(g_instance, &sessionInfo, &g_session),
                   "xrCreateSession (OpenGL Win32 binding)")) {
        return false;
    }

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.0f;
    if (!Succeeded(::xrCreateReferenceSpace(g_session, &spaceInfo, &g_local_space),
                   "xrCreateReferenceSpace (LOCAL)")) {
        return false;
    }

    XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(::xrGetInstanceProperties(g_instance, &properties))) {
        H47VR_LOG_INFO("openxr: runtime %s", properties.runtimeName);
    }
    if (CreatePresentationSwapchains()) {
        H47VR_LOG_INFO("openxr: headset presentation ready - theater uses one "
                       "quad, gameplay uses two projection views");
    } else {
        H47VR_LOG_WARN("openxr: presentation unavailable; pose-only fallback");
    }
    return true;
}

bool AcquireEyeImage(int eye, uint32_t* imageIndex) {
    XrSwapchainImageAcquireInfo acquireInfo{
        XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrResult result = ::xrAcquireSwapchainImage(
        g_eyes[eye].handle, &acquireInfo, imageIndex);
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: acquire eye %d image failed (%d)", eye,
                       static_cast<int>(result));
        return false;
    }
    g_acquired_images[eye] = *imageIndex;

    XrSwapchainImageWaitInfo waitInfo{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    waitInfo.timeout = XR_INFINITE_DURATION;
    result = ::xrWaitSwapchainImage(g_eyes[eye].handle, &waitInfo);
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: wait eye %d image failed (%d)", eye,
                       static_cast<int>(result));
        return false;
    }
    return true;
}

void ReleaseAcquiredImages() {
    XrSwapchainImageReleaseInfo releaseInfo{
        XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    for (int eye = 0; eye < 2; ++eye) {
        if (g_acquired_images[eye] == UINT32_MAX) {
            continue;
        }
        const XrResult result =
            ::xrReleaseSwapchainImage(g_eyes[eye].handle, &releaseInfo);
        if (XR_FAILED(result)) {
            H47VR_LOG_WARN("openxr: release eye %d image failed (%d)", eye,
                           static_cast<int>(result));
        }
        g_acquired_images[eye] = UINT32_MAX;
    }
}

bool BlitBackbufferToEye(int eye, uint32_t imageIndex, int sourceLeft,
                         int sourceRight, int sourceHeight) {
    GLint previousRead = 0;
    GLint previousDraw = 0;
    g_gl_get_integer(kGlReadFramebufferBinding, &previousRead);
    g_gl_get_integer(kGlDrawFramebufferBinding, &previousDraw);

    g_gl_bind_framebuffer(kGlReadFramebuffer,
                          static_cast<GLuint>(previousRead));
    g_gl_bind_framebuffer(kGlDrawFramebuffer, g_eyes[eye].framebuffers[imageIndex]);
    g_gl_blit_framebuffer(sourceLeft, 0, sourceRight, sourceHeight, 0, 0,
                          static_cast<GLint>(g_eyes[eye].width),
                          static_cast<GLint>(g_eyes[eye].height),
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);

    g_gl_bind_framebuffer(kGlReadFramebuffer,
                          static_cast<GLuint>(previousRead));
    g_gl_bind_framebuffer(kGlDrawFramebuffer,
                          static_cast<GLuint>(previousDraw));
    return true;
}

void SetSubImage(XrSwapchainSubImage* subImage, int eye) {
    subImage->swapchain = g_eyes[eye].handle;
    subImage->imageRect.offset = {0, 0};
    subImage->imageRect.extent = {
        static_cast<int32_t>(g_eyes[eye].width),
        static_cast<int32_t>(g_eyes[eye].height),
    };
    subImage->imageArrayIndex = 0;
}

const XrCompositionLayerBaseHeader* SubmitPresentation(
    const XrView views[2], StereoMode mode, bool directStereo) {
    int sourceWidth = 0;
    int sourceHeight = 0;
    if (!directStereo && !GlStereoSourceSize(&sourceWidth, &sourceHeight)) {
        return nullptr;
    }

    static XrCompositionLayerProjection projection{
        XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    static XrCompositionLayerProjectionView projectionViews[2] = {
        XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
        XrCompositionLayerProjectionView{
            XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW},
    };
    static XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

    if (mode == StereoMode::Stereo) {
        for (int eye = 0; eye < 2; ++eye) {
            if (!directStereo) {
                uint32_t imageIndex = 0;
                if (!AcquireEyeImage(eye, &imageIndex)) {
                    return nullptr;
                }
                const int left = eye == 0 ? 0 : sourceWidth / 2;
                const int right = eye == 0 ? sourceWidth / 2 : sourceWidth;
                BlitBackbufferToEye(eye, imageIndex, left, right, sourceHeight);
            }

            projectionViews[eye].type =
                XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
            projectionViews[eye].next = nullptr;
            projectionViews[eye].pose = views[eye].pose;
            projectionViews[eye].fov = views[eye].fov;
            SetSubImage(&projectionViews[eye].subImage, eye);
        }
        projection.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION;
        projection.next = nullptr;
        projection.layerFlags = 0;
        projection.space = g_local_space;
        projection.viewCount = 2;
        projection.views = projectionViews;
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    }

    if (mode == StereoMode::Theater) {
        uint32_t imageIndex = 0;
        if (!AcquireEyeImage(0, &imageIndex)) {
            return nullptr;
        }
        BlitBackbufferToEye(0, imageIndex, 0, sourceWidth, sourceHeight);
        quad.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
        quad.next = nullptr;
        quad.layerFlags = 0;
        quad.space = g_local_space;
        quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        SetSubImage(&quad.subImage, 0);
        const XrQuaternionf& head = views[0].pose.orientation;
        const float forwardX = -2.0f * (head.x * head.z + head.w * head.y);
        const float forwardY = -2.0f * (head.y * head.z - head.w * head.x);
        const float forwardZ =
            -(1.0f - 2.0f * (head.x * head.x + head.y * head.y));
        constexpr float kTheaterDistance = 1.8f;
        quad.pose.orientation = head;
        quad.pose.position = {
            views[0].pose.position.x + forwardX * kTheaterDistance,
            views[0].pose.position.y + forwardY * kTheaterDistance,
            views[0].pose.position.z + forwardZ * kTheaterDistance,
        };
        quad.size = {1.6f, 1.2f};
        return reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
    }
    return nullptr;
}

XrVector3f RotateByInverseQuaternion(const XrQuaternionf& q,
                                     const XrVector3f& v) {
    // v' = v + 2*w*(u x v) + 2*(u x (u x v)), where u is the inverse
    // rotation axis (-q.xyz) and w is unchanged for a unit quaternion.
    const float ux = -q.x;
    const float uy = -q.y;
    const float uz = -q.z;
    const float crossX = uy * v.z - uz * v.y;
    const float crossY = uz * v.x - ux * v.z;
    const float crossZ = ux * v.y - uy * v.x;
    const float cross2X = uy * crossZ - uz * crossY;
    const float cross2Y = uz * crossX - ux * crossZ;
    const float cross2Z = ux * crossY - uy * crossX;
    return {
        v.x + 2.0f * (q.w * crossX + cross2X),
        v.y + 2.0f * (q.w * crossY + cross2Y),
        v.z + 2.0f * (q.w * crossZ + cross2Z),
    };
}

bool StartRenderFrame() {
    if (g_render_frame_open) {
        return true;
    }

    InvalidatePose();
    ReleaseAcquiredImages();

    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = ::xrWaitFrame(g_session, &waitInfo, &g_render_frame);
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: xrWaitFrame failed (%d)",
                       static_cast<int>(result));
        return false;
    }

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    result = ::xrBeginFrame(g_session, &beginInfo);
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: xrBeginFrame failed (%d)",
                       static_cast<int>(result));
        return false;
    }
    g_render_frame_open = true;
    g_direct_stereo = false;
    g_render_views_valid = false;

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = g_render_frame.predictedDisplayTime;
    locateInfo.space = g_local_space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 2;
    g_render_views[0] = XrView{XR_TYPE_VIEW};
    g_render_views[1] = XrView{XR_TYPE_VIEW};
    result = ::xrLocateViews(g_session, &locateInfo, &viewState, 2, &viewCount,
                             g_render_views);

    g_render_views_valid =
        XR_SUCCEEDED(result) && viewCount >= 2 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0;
    if (!g_render_views_valid) {
        return true;
    }

    OpenXrEyePose sample;
    sample.valid = true;
    sample.frame = g_render_frame.predictedDisplayTime;
    sample.fovValid = true;
    const XrVector3f midpoint = {
        (g_render_views[0].pose.position.x +
         g_render_views[1].pose.position.x) *
            0.5f,
        (g_render_views[0].pose.position.y +
         g_render_views[1].pose.position.y) *
            0.5f,
        (g_render_views[0].pose.position.z +
         g_render_views[1].pose.position.z) *
            0.5f,
    };
    for (int eye = 0; eye < 2; ++eye) {
        const XrVector3f relative = {
            g_render_views[eye].pose.position.x - midpoint.x,
            g_render_views[eye].pose.position.y - midpoint.y,
            g_render_views[eye].pose.position.z - midpoint.z,
        };
        const XrVector3f headLocal = RotateByInverseQuaternion(
            g_render_views[0].pose.orientation, relative);
        sample.eyeXMetres[eye] = headLocal.x;
        sample.fov[eye][0] = g_render_views[eye].fov.angleLeft;
        sample.fov[eye][1] = g_render_views[eye].fov.angleRight;
        sample.fov[eye][2] = g_render_views[eye].fov.angleUp;
        sample.fov[eye][3] = g_render_views[eye].fov.angleDown;
        sample.fovValid = sample.fovValid && ValidFov(g_render_views[eye].fov);
    }

    const XrQuaternionf& q = g_render_views[0].pose.orientation;
    const float forwardX = -2.0f * (q.x * q.z + q.w * q.y);
    const float forwardY = -2.0f * (q.y * q.z - q.w * q.x);
    const float forwardZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    sample.yawDegrees = std::atan2(forwardX, -forwardZ) * 57.29577951308232f;
    sample.pitchDegrees =
        std::atan2(forwardY,
                   std::sqrt(forwardX * forwardX + forwardZ * forwardZ)) *
        57.29577951308232f;

    const float headUpX = 2.0f * (q.x * q.y - q.w * q.z);
    const float headUpY = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    const float headUpZ = 2.0f * (q.y * q.z + q.w * q.x);
    float upX = -forwardX * forwardY;
    float upY = 1.0f - forwardY * forwardY;
    float upZ = -forwardZ * forwardY;
    const float upLength = std::sqrt(upX * upX + upY * upY + upZ * upZ);
    if (upLength > 1e-5f) {
        upX /= upLength;
        upY /= upLength;
        upZ /= upLength;
        const float crossX = upY * headUpZ - upZ * headUpY;
        const float crossY = upZ * headUpX - upX * headUpZ;
        const float crossZ = upX * headUpY - upY * headUpX;
        const float roll =
            std::atan2(crossX * forwardX + crossY * forwardY +
                           crossZ * forwardZ,
                       upX * headUpX + upY * headUpY + upZ * headUpZ);
        sample.rollDegrees = roll * 57.29577951308232f;
    }

    std::lock_guard<std::mutex> lock(g_pose_mutex);
    g_pose = sample;
    return true;
}

void ClearDirectTargets() {
    GLint previousFramebuffer = 0;
    g_gl_get_integer(kGlDrawFramebufferBinding, &previousFramebuffer);
    g_gl_push_attrib(kGlAllAttribBits);
    g_gl_disable(kGlScissorTest);
    g_gl_color_mask(1, 1, 1, 1);
    g_gl_depth_mask(1);
    g_gl_clear_color(0.0f, 0.0f, 0.0f, 1.0f);
    for (int eye = 0; eye < 2; ++eye) {
        const uint32_t imageIndex = g_acquired_images[eye];
        if (imageIndex == UINT32_MAX) {
            continue;
        }
        g_gl_bind_framebuffer(kGlFramebuffer,
                              g_eyes[eye].framebuffers[imageIndex]);
        g_gl_clear(kGlColorBufferBit | kGlDepthBufferBit);
    }
    g_gl_bind_framebuffer(kGlFramebuffer,
                          static_cast<GLuint>(previousFramebuffer));
    g_gl_pop_attrib();
}

void MeasureDirectTargets() {
    if (++g_direct_frame_count != 120 || g_gl_read_pixels == nullptr) {
        return;
    }
    constexpr int kSampleSize = 256;
    std::vector<float> pixels[2];
    float mean[2] = {};
    float peak[2] = {};
    const GLint x = (std::max)(
        0, (static_cast<int>(g_eyes[0].width) - kSampleSize) / 2);
    const GLint y = (std::max)(
        0, (static_cast<int>(g_eyes[0].height) - kSampleSize) / 2);

    GLint previousFramebuffer = 0;
    g_gl_get_integer(kGlDrawFramebufferBinding, &previousFramebuffer);
    for (int eye = 0; eye < 2; ++eye) {
        pixels[eye].assign(kSampleSize * kSampleSize * 4, 0.0f);
        const uint32_t imageIndex = g_acquired_images[eye];
        if (imageIndex == UINT32_MAX) {
            continue;
        }
        g_gl_bind_framebuffer(kGlFramebuffer,
                              g_eyes[eye].framebuffers[imageIndex]);
        g_gl_read_pixels(x, y, kSampleSize, kSampleSize, GL_RGBA, GL_FLOAT,
                         pixels[eye].data());
        for (int i = 0; i < kSampleSize * kSampleSize; ++i) {
            const float value =
                (pixels[eye][i * 4] + pixels[eye][i * 4 + 1] +
                 pixels[eye][i * 4 + 2]) /
                3.0f;
            mean[eye] += value;
            peak[eye] = (std::max)(peak[eye], value);
        }
        mean[eye] /= static_cast<float>(kSampleSize * kSampleSize);
    }
    g_gl_bind_framebuffer(kGlFramebuffer,
                          static_cast<GLuint>(previousFramebuffer));

    float difference = 0.0f;
    for (int i = 0; i < kSampleSize * kSampleSize; ++i) {
        difference +=
            (std::fabs(pixels[0][i * 4] - pixels[1][i * 4]) +
             std::fabs(pixels[0][i * 4 + 1] - pixels[1][i * 4 + 1]) +
             std::fabs(pixels[0][i * 4 + 2] - pixels[1][i * 4 + 2])) /
            3.0f;
    }
    difference /= static_cast<float>(kSampleSize * kSampleSize);
    H47VR_LOG_INFO("openxr: direct image check mean L/R %.4f/%.4f, peak "
                   "L/R %.4f/%.4f, left/right difference %.4f",
                   mean[0], mean[1], peak[0], peak[1], difference);
}

void FinishRenderFrame(StereoMode mode) {
    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = g_render_frame.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 0;
    endInfo.layers = nullptr;
    if (g_presentation_ready && g_render_views_valid &&
        mode != StereoMode::Off) {
        const bool direct = g_direct_stereo && mode == StereoMode::Stereo;
        const XrCompositionLayerBaseHeader* layer =
            SubmitPresentation(g_render_views, mode, direct);
        if (direct) {
            MeasureDirectTargets();
        }
        if (layer != nullptr) {
            endInfo.layerCount = 1;
            endInfo.layers = &layer;
            const int submittedMode =
                direct ? 3 : static_cast<int>(mode);
            if (submittedMode != g_last_submitted_mode) {
                g_last_submitted_mode = submittedMode;
                H47VR_LOG_INFO("openxr: submitting %s layer",
                               direct ? "direct per-eye stereo projection"
                                      : (mode == StereoMode::Stereo
                                             ? "copied stereo projection"
                                             : "theater quad"));
            }
        }
    } else {
        g_last_submitted_mode = -1;
    }
    ReleaseAcquiredImages();
    ::xrEndFrame(g_session, &endInfo);
    g_render_frame_open = false;
    g_direct_stereo = false;
    g_render_views_valid = false;
}

}  // namespace

bool OpenXrBeginStereoTarget(OpenXrStereoTarget* target) {
    if (target == nullptr || !g_presentation_ready ||
        !g_session_running.load()) {
        return false;
    }
    if (g_direct_stereo) {
        for (int eye = 0; eye < 2; ++eye) {
            const uint32_t imageIndex = g_acquired_images[eye];
            if (imageIndex == UINT32_MAX) {
                return false;
            }
            target->framebuffer[eye] = g_eyes[eye].framebuffers[imageIndex];
        }
        target->width = static_cast<int>(g_eyes[0].width);
        target->height = static_cast<int>(g_eyes[0].height);
        return true;
    }

    PollEvents();
    if (!g_session_running.load() || !StartRenderFrame() ||
        !g_render_views_valid) {
        return false;
    }

    if (!g_direct_stereo) {
        ReleaseAcquiredImages();
        for (int eye = 0; eye < 2; ++eye) {
            uint32_t imageIndex = 0;
            if (!AcquireEyeImage(eye, &imageIndex)) {
                ReleaseAcquiredImages();
                return false;
            }
        }
        g_direct_stereo = true;
        ClearDirectTargets();
        static bool reported = false;
        if (!reported) {
            reported = true;
            H47VR_LOG_INFO("openxr: direct per-eye stereo render targets "
                           "ready at %ux%u",
                           g_eyes[0].width, g_eyes[0].height);
        }
    }

    for (int eye = 0; eye < 2; ++eye) {
        const uint32_t imageIndex = g_acquired_images[eye];
        if (imageIndex == UINT32_MAX) {
            return false;
        }
        target->framebuffer[eye] = g_eyes[eye].framebuffers[imageIndex];
    }
    target->width = static_cast<int>(g_eyes[0].width);
    target->height = static_cast<int>(g_eyes[0].height);
    return true;
}

void OpenXrCancelStereoTarget() {
    ReleaseAcquiredImages();
    g_direct_stereo = false;
}

bool OpenXrDirectStereoActive() {
    return g_direct_stereo;
}

bool OpenXrPoseStart() {
    g_gate_path = JoinPath(mod::GameDirectory(), L"h47vr-openxr.txt");
    g_runtime_path =
        JoinPath(mod::GameDirectory(), L"h47vr-openxr-runtime.txt");
    ApplyRuntimeOverride();
    H47VR_LOG_INFO("openxr pose backend: ready - create %s to enable pose tests",
                   Narrow(g_gate_path).c_str());
    return true;
}

void OpenXrPoseFrame(HDC device, unsigned long frame) {
    if (g_gate_path.empty()) {
        return;
    }
    const bool wanted =
        ::GetFileAttributesW(g_gate_path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!wanted) {
        LookDriverSetHeadPose(false, 0.0, 0.0);
        if (g_attempted.load() || g_instance != XR_NULL_HANDLE) {
            ShutdownXr();
            H47VR_LOG_INFO("openxr: pose and presentation backend stopped");
        }
        return;
    }

    if (!g_attempted.load()) {
        ApplyRuntimeOverride();
        if (!Initialize(device)) {
            LookDriverSetHeadPose(false, 0.0, 0.0);
            ShutdownXr();
            g_attempted = true;
            H47VR_LOG_WARN("openxr: pose backend unavailable; fixed IPD remains active");
            return;
        }
        g_attempted = true;
    }

    if (g_session == XR_NULL_HANDLE) {
        InvalidatePose();
        LookDriverSetHeadPose(false, 0.0, 0.0);
        return;
    }
    PollEvents();
    if (!g_session_running.load()) {
        InvalidatePose();
        LookDriverSetHeadPose(false, 0.0, 0.0);
        return;
    }
    if (!g_render_frame_open && !StartRenderFrame()) {
        LookDriverSetHeadPose(false, 0.0, 0.0);
        return;
    }
    OpenXrEyePose pose;
    const bool tracked = OpenXrPoseGet(&pose);
    LookDriverSetHeadPose(tracked, pose.yawDegrees, pose.pitchDegrees);
    FinishRenderFrame(StereoRendererMode());
    if (frame % 300 == 0) {
        if (tracked) {
            H47VR_LOG_INFO("openxr: eye X %.4f / %.4f m, head yaw %.2f pitch %.2f, "
                           "FOV L/R/U/D %.2f/%.2f/%.2f/%.2f deg",
                           pose.eyeXMetres[0], pose.eyeXMetres[1],
                           pose.yawDegrees, pose.pitchDegrees,
                           pose.fov[0][0] * 57.29577951308232f,
                           pose.fov[0][1] * 57.29577951308232f,
                           pose.fov[0][2] * 57.29577951308232f,
                           pose.fov[0][3] * 57.29577951308232f);
        }
    }
}

bool OpenXrPoseGet(OpenXrEyePose* pose) {
    if (pose == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_pose_mutex);
    *pose = g_pose;
    return pose->valid;
}

}  // namespace h47vr
