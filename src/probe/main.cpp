// OpenXR graphics-bridge probe.
//
// Answers one question: on this machine, can a 32-bit process hold a legacy
// Windows OpenGL context and an OpenXR session at the same time?
//
// Every step reports its result and the probe keeps going after a failure, so a
// single run shows how far the bridge actually gets.

#include <windows.h>
#include <GL/gl.h>

#define XR_USE_PLATFORM_WIN32 1
#define XR_USE_GRAPHICS_API_OPENGL 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Result(const char* step, XrResult result) {
    if (XR_SUCCEEDED(result)) {
        std::printf("  [ ok ] %s\n", step);
    } else {
        ++g_failures;
        std::printf("  [FAIL] %s (XrResult %d)\n", step, static_cast<int>(result));
    }
}

void Info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::printf("         ");
    std::vprintf(format, args);
    std::printf("\n");
    va_end(args);
}

void Section(const char* title) {
    std::printf("\n%s\n", title);
}

bool HasExtension(const std::vector<XrExtensionProperties>& all, const char* name) {
    for (const XrExtensionProperties& properties : all) {
        if (std::strcmp(properties.extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

// A minimal window and WGL context, standing in for what RenderOpenGL.dll does.
struct GlContext {
    HWND window = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    bool ok = false;
};

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return ::DefWindowProcW(window, message, wparam, lparam);
}

GlContext CreateGlContext() {
    GlContext context;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &WindowProc;
    windowClass.hInstance = ::GetModuleHandleW(nullptr);
    windowClass.lpszClassName = L"h47vr-probe";
    if (::RegisterClassExW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        std::printf("  [FAIL] RegisterClassExW (%lu)\n", ::GetLastError());
        return context;
    }

    context.window = ::CreateWindowExW(0, L"h47vr-probe", L"h47vr probe", WS_OVERLAPPEDWINDOW,
                                       0, 0, 64, 64, nullptr, nullptr,
                                       windowClass.hInstance, nullptr);
    if (context.window == nullptr) {
        std::printf("  [FAIL] CreateWindowExW (%lu)\n", ::GetLastError());
        return context;
    }

    context.dc = ::GetDC(context.window);

    PIXELFORMATDESCRIPTOR format{};
    format.nSize = sizeof(format);
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 32;
    format.cDepthBits = 24;
    format.iLayerType = PFD_MAIN_PLANE;

    const int pixelFormat = ::ChoosePixelFormat(context.dc, &format);
    if (pixelFormat == 0) {
        std::printf("  [FAIL] ChoosePixelFormat (%lu)\n", ::GetLastError());
        return context;
    }
    if (!::SetPixelFormat(context.dc, pixelFormat, &format)) {
        std::printf("  [FAIL] SetPixelFormat (%lu)\n", ::GetLastError());
        return context;
    }

    context.rc = ::wglCreateContext(context.dc);
    if (context.rc == nullptr) {
        std::printf("  [FAIL] wglCreateContext (%lu)\n", ::GetLastError());
        return context;
    }
    if (!::wglMakeCurrent(context.dc, context.rc)) {
        std::printf("  [FAIL] wglMakeCurrent (%lu)\n", ::GetLastError());
        return context;
    }

    context.ok = true;
    return context;
}

void DestroyGlContext(GlContext& context) {
    if (context.rc != nullptr) {
        ::wglMakeCurrent(nullptr, nullptr);
        ::wglDeleteContext(context.rc);
    }
    if (context.dc != nullptr && context.window != nullptr) {
        ::ReleaseDC(context.window, context.dc);
    }
    if (context.window != nullptr) {
        ::DestroyWindow(context.window);
    }
}

}  // namespace

int main() {
    std::printf("h47vr OpenXR graphics-bridge probe\n");
    std::printf("process: %s\n",
                sizeof(void*) == 4 ? "32-bit x86" : "64-bit (control run)");

    // --- 1. what does the active runtime advertise? -------------------------
    Section("1. instance extensions");
    uint32_t extensionCount = 0;
    XrResult result = ::xrEnumerateInstanceExtensionProperties(nullptr, 0, &extensionCount, nullptr);
    Result("xrEnumerateInstanceExtensionProperties (count)", result);
    if (XR_FAILED(result)) {
        std::printf("\nCannot reach a runtime at all; stopping.\n");
        return 1;
    }

    std::vector<XrExtensionProperties> extensions(extensionCount, XrExtensionProperties{XR_TYPE_EXTENSION_PROPERTIES});
    result = ::xrEnumerateInstanceExtensionProperties(nullptr, extensionCount, &extensionCount, extensions.data());
    Result("xrEnumerateInstanceExtensionProperties (list)", result);
    Info("%u extensions", extensionCount);

    const bool hasOpenGl = HasExtension(extensions, "XR_KHR_opengl_enable");
    const bool hasD3D11 = HasExtension(extensions, "XR_KHR_D3D11_enable");
    Info("XR_KHR_opengl_enable : %s", hasOpenGl ? "yes" : "NO");
    Info("XR_KHR_D3D11_enable  : %s", hasD3D11 ? "yes" : "no");

    // --- 2. instance ---------------------------------------------------------
    Section("2. instance");
    std::vector<const char*> enabled;
    if (hasOpenGl) {
        enabled.push_back("XR_KHR_opengl_enable");
    }

    XrInstance instance = XR_NULL_HANDLE;
    XrInstanceCreateInfo createInfo{XR_TYPE_INSTANCE_CREATE_INFO};
    ::strcpy_s(createInfo.applicationInfo.applicationName, "h47vr probe");
    createInfo.applicationInfo.applicationVersion = 1;
    ::strcpy_s(createInfo.applicationInfo.engineName, "none");
    createInfo.enabledExtensionCount = static_cast<uint32_t>(enabled.size());
    createInfo.enabledExtensionNames = enabled.data();
    result = ::xrCreateInstance(&createInfo, &instance);
    Result(hasOpenGl ? "xrCreateInstance (with XR_KHR_opengl_enable)"
                     : "xrCreateInstance (OpenGL extension unavailable)", result);

    if (XR_FAILED(result) && hasOpenGl) {
        // Separate "the runtime cannot initialise at all" from "the OpenGL
        // binding specifically is refused", which are different problems.
        std::printf("         retrying without any graphics extension\n");
        createInfo.enabledExtensionCount = 0;
        createInfo.enabledExtensionNames = nullptr;
        result = ::xrCreateInstance(&createInfo, &instance);
        Result("xrCreateInstance (no extensions, no graphics binding)", result);
        if (XR_SUCCEEDED(result)) {
            std::printf("         -> the runtime initialises; XR_KHR_opengl_enable is what it refused\n");
        }
    }
    if (XR_FAILED(result)) {
        std::printf("\nNo instance; stopping.\n");
        return 1;
    }

    XrInstanceProperties instanceProperties{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(::xrGetInstanceProperties(instance, &instanceProperties))) {
        Info("runtime      : %s", instanceProperties.runtimeName);
        Info("runtime ver  : %u.%u.%u", XR_VERSION_MAJOR(instanceProperties.runtimeVersion),
             XR_VERSION_MINOR(instanceProperties.runtimeVersion),
             XR_VERSION_PATCH(instanceProperties.runtimeVersion));
    }

    // --- 3. headset ----------------------------------------------------------
    Section("3. system");
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    result = ::xrGetSystem(instance, &systemInfo, &systemId);
    Result("xrGetSystem (head mounted display)", result);
    if (XR_FAILED(result)) {
        std::printf("\nNo headset available. Start Virtual Desktop and connect the Quest, then retry.\n");
        ::xrDestroyInstance(instance);
        return 2;
    }

    XrSystemProperties systemProperties{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(::xrGetSystemProperties(instance, systemId, &systemProperties))) {
        Info("system       : %s", systemProperties.systemName);
        Info("vendor id    : %u", systemProperties.vendorId);
    }

    // --- 4. view configuration ----------------------------------------------
    Section("4. view configuration");
    uint32_t viewConfigCount = 0;
    result = ::xrEnumerateViewConfigurations(instance, systemId, 0, &viewConfigCount, nullptr);
    if (XR_SUCCEEDED(result) && viewConfigCount > 0) {
        std::vector<XrViewConfigurationType> configs(viewConfigCount);
        ::xrEnumerateViewConfigurations(instance, systemId, viewConfigCount, &viewConfigCount, configs.data());
        Info("%u view configurations", viewConfigCount);

        uint32_t viewCount = 0;
        result = ::xrEnumerateViewConfigurationViews(instance, systemId,
                                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                     0, &viewCount, nullptr);
        Result("xrEnumerateViewConfigurationViews", result);
        if (XR_SUCCEEDED(result) && viewCount > 0) {
            std::vector<XrViewConfigurationView> views(
                viewCount, XrViewConfigurationView{XR_TYPE_VIEW_CONFIGURATION_VIEW});
            if (XR_SUCCEEDED(::xrEnumerateViewConfigurationViews(
                    instance, systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                    viewCount, &viewCount, views.data()))) {
                for (uint32_t i = 0; i < viewCount; ++i) {
                    Info("eye %u        : %ux%u recommended, %ux%u max", i,
                         views[i].recommendedImageRectWidth,
                         views[i].recommendedImageRectHeight,
                         views[i].maxImageRectWidth, views[i].maxImageRectHeight);
                }
            }
        }
    }

    // --- 5. legacy WGL context ----------------------------------------------
    Section("5. legacy WGL context (what the game uses)");
    GlContext gl = CreateGlContext();
    if (!gl.ok) {
        std::printf("\nCould not create a WGL context; stopping before the session test.\n");
        ::xrDestroyInstance(instance);
        return 3;
    }
    Result("wglCreateContext + wglMakeCurrent", XR_SUCCESS);
    Info("GL_VERSION   : %s",
         reinterpret_cast<const char*>(::glGetString(GL_VERSION)));
    Info("GL_RENDERER  : %s",
         reinterpret_cast<const char*>(::glGetString(GL_RENDERER)));

    // --- 6. OpenGL graphics requirements ------------------------------------
    Section("6. runtime OpenGL requirements");
    PFN_xrGetOpenGLGraphicsRequirementsKHR getRequirements = nullptr;
    ::xrGetInstanceProcAddr(instance, "xrGetOpenGLGraphicsRequirementsKHR",
                            reinterpret_cast<PFN_xrVoidFunction*>(&getRequirements));
    if (getRequirements == nullptr) {
        std::printf("  [FAIL] xrGetOpenGLGraphicsRequirementsKHR is unavailable\n");
        ++g_failures;
    } else {
        XrGraphicsRequirementsOpenGLKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
        result = getRequirements(instance, systemId, &requirements);
        Result("xrGetOpenGLGraphicsRequirementsKHR", result);
        if (XR_SUCCEEDED(result)) {
            Info("min GL       : %u.%u.%u",
                 XR_VERSION_MAJOR(requirements.minApiVersionSupported),
                 XR_VERSION_MINOR(requirements.minApiVersionSupported),
                 XR_VERSION_PATCH(requirements.minApiVersionSupported));
            Info("max GL       : %u.%u.%u",
                 XR_VERSION_MAJOR(requirements.maxApiVersionSupported),
                 XR_VERSION_MINOR(requirements.maxApiVersionSupported),
                 XR_VERSION_PATCH(requirements.maxApiVersionSupported));
        }
    }

    // --- 7. the actual gate: a session over that context --------------------
    Section("7. session over the legacy context (the gate)");
    XrGraphicsBindingOpenGLWin32KHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    binding.hDC = gl.dc;
    binding.hGLRC = gl.rc;

    XrSession session = XR_NULL_HANDLE;
    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = systemId;
    result = ::xrCreateSession(instance, &sessionInfo, &session);
    Result("xrCreateSession (OpenGL Win32 binding)", result);

    bool sessionOk = XR_SUCCEEDED(result);
    if (sessionOk) {
        Info("the graphics bridge works on this machine");

        // Prove the session reaches a usable state rather than merely existing.
        XrSessionState state = XR_SESSION_STATE_UNKNOWN;
        for (int i = 0; i < 200; ++i) {
            XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
            while (::xrPollEvent(instance, &event) == XR_SUCCESS) {
                if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    state = reinterpret_cast<XrEventDataSessionStateChanged*>(&event)->state;
                }
                event = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
            }
            if (state == XR_SESSION_STATE_READY || state == XR_SESSION_STATE_FOCUSED) {
                break;
            }
            ::Sleep(10);
        }
        Info("session state after polling: %d", static_cast<int>(state));
        if (state == XR_SESSION_STATE_READY || state == XR_SESSION_STATE_FOCUSED) {
            const XrSessionBeginInfo beginInfo{XR_TYPE_SESSION_BEGIN_INFO,
                                               nullptr,
                                               XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
            XrResult begin = ::xrBeginSession(session, &beginInfo);
            Result("xrBeginSession", begin);
        }
        ::xrDestroySession(session);
    }

    DestroyGlContext(gl);
    ::xrDestroyInstance(instance);

    Section("summary");
    if (sessionOk) {
        std::printf("GATE PASSED: OpenXR and the legacy OpenGL context coexist in this 32-bit process.\n");
        return 0;
    }
    std::printf("GATE FAILED: %d step(s) did not succeed. The D3D11 sharing fallback is next.\n", g_failures);
    return 4;
}
