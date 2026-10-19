add_library(h47vr_core STATIC
    src/console_driver.cpp
    src/paths.cpp
    src/log.cpp
    src/fingerprint.cpp
    src/gl_probe.cpp
    src/camera_probe.cpp
    src/camera_readback.cpp
    src/first_person_camera.cpp
    src/look_driver.cpp
    src/stereo_renderer.cpp
    src/window_keepalive.cpp
    src/symbols.cpp
    src/hooks.cpp
    src/renderer_probe.cpp
    src/scene_probe.cpp
    src/openxr_pose.cpp
    src/mod.cpp)

target_include_directories(h47vr_core
    PUBLIC include
    PRIVATE "${H47VR_OPENXR_ROOT}/include")

target_link_libraries(h47vr_core
    PUBLIC h47vr_pins_headers
    PRIVATE bcrypt h47vr_minhook "${H47VR_OPENXR_LIB}")

if(MSVC)
    target_compile_options(h47vr_core PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
