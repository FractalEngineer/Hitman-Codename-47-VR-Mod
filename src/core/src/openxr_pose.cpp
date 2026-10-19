#include "h47vr/openxr_pose.hpp"

#include "h47vr/log.hpp"
#include "h47vr/mod.hpp"
#include "h47vr/paths.hpp"
#include "h47vr/symbols.hpp"

#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_GRAPHICS_API_OPENGL 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <vector>

namespace h47vr {
namespace {

const char* kOpenGlExtension = "XR_KHR_opengl_enable";

std::wstring g_gate_path;
std::atomic<bool> g_attempted{false};
std::atomic<bool> g_session_running{false};
std::mutex g_pose_mutex;
OpenXrEyePose g_pose;

XrInstance g_instance = XR_NULL_HANDLE;
XrSystemId g_system = XR_NULL_SYSTEM_ID;
XrSession g_session = XR_NULL_HANDLE;
XrSpace g_local_space = XR_NULL_HANDLE;
XrSessionState g_session_state = XR_SESSION_STATE_UNKNOWN;

bool Succeeded(XrResult result, const char* step) {
    if (XR_SUCCEEDED(result)) {
        H47VR_LOG_INFO("openxr: %s", step);
        return true;
    }
    H47VR_LOG_WARN("openxr: %s failed (XrResult %d)", step,
                   static_cast<int>(result));
    return false;
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
    H47VR_LOG_WARN("openxr: pose-only test mode; no swapchain is submitted");
    return true;
}

void LocateViews() {
    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
    XrResult result = ::xrWaitFrame(g_session, &waitInfo, &frameState);
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: xrWaitFrame failed (%d)", static_cast<int>(result));
        return;
    }

    XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
    result = ::xrBeginFrame(g_session, &beginInfo);
    if (XR_FAILED(result)) {
        H47VR_LOG_WARN("openxr: xrBeginFrame failed (%d)", static_cast<int>(result));
        return;
    }

    XrViewLocateInfo locateInfo{XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = g_local_space;
    XrViewState viewState{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 2;
    XrView views[2] = {
        XrView{XR_TYPE_VIEW},
        XrView{XR_TYPE_VIEW},
    };
    result = ::xrLocateViews(g_session, &locateInfo, &viewState, 2, &viewCount,
                             views);

    if (XR_SUCCEEDED(result) && viewCount >= 2 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) != 0 &&
        (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) != 0) {
        OpenXrEyePose sample;
        sample.valid = true;
        sample.frame = frameState.predictedDisplayTime;
        for (int eye = 0; eye < 2; ++eye) {
            sample.eyeXMetres[eye] = views[eye].pose.position.x;
            sample.fov[eye][0] = views[eye].fov.angleLeft;
            sample.fov[eye][1] = views[eye].fov.angleRight;
            sample.fov[eye][2] = views[eye].fov.angleUp;
            sample.fov[eye][3] = views[eye].fov.angleDown;
        }

        const XrQuaternionf& q = views[0].pose.orientation;
        const float forwardX = -2.0f * (q.x * q.z + q.w * q.y);
        const float forwardY = -2.0f * (q.y * q.z - q.w * q.x);
        const float forwardZ = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
        sample.yawDegrees =
            std::atan2(forwardX, -forwardZ) * 57.29577951308232f;
        sample.pitchDegrees =
            std::atan2(forwardY,
                       std::sqrt(forwardX * forwardX + forwardZ * forwardZ)) *
            57.29577951308232f;

        std::lock_guard<std::mutex> lock(g_pose_mutex);
        g_pose = sample;
    }

    XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 0;
    endInfo.layers = nullptr;
    ::xrEndFrame(g_session, &endInfo);
}

}  // namespace

bool OpenXrPoseStart() {
    g_gate_path = JoinPath(mod::GameDirectory(), L"h47vr-openxr.txt");
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
        {
            std::lock_guard<std::mutex> lock(g_pose_mutex);
            g_pose.valid = false;
        }
        // Allow a failed no-headset attempt to be retried after the tool is
        // toggled without restarting the game.
        if (g_session == XR_NULL_HANDLE) {
            if (g_instance != XR_NULL_HANDLE) {
                ::xrDestroyInstance(g_instance);
                g_instance = XR_NULL_HANDLE;
            }
            g_attempted = false;
        }
        return;
    }

    if (!g_attempted.load()) {
        if (!Initialize(device)) {
            g_attempted = true;
            H47VR_LOG_WARN("openxr: pose backend unavailable; fixed IPD remains active");
            return;
        }
        g_attempted = true;
    }

    if (g_session == XR_NULL_HANDLE) {
        return;
    }
    PollEvents();
    if (!g_session_running.load()) {
        return;
    }
    LocateViews();
    if (frame % 300 == 0) {
        OpenXrEyePose pose;
        if (OpenXrPoseGet(&pose)) {
            H47VR_LOG_INFO("openxr: eye X %.4f / %.4f m, head yaw %.2f pitch %.2f",
                           pose.eyeXMetres[0], pose.eyeXMetres[1],
                           pose.yawDegrees, pose.pitchDegrees);
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
