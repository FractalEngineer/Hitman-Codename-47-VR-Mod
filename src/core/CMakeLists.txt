add_library(h47vr_core STATIC
    src/console_driver.cpp
    src/paths.cpp
    src/log.cpp
    src/fingerprint.cpp
    src/gl_probe.cpp
    src/camera_probe.cpp
    src/symbols.cpp
    src/hooks.cpp
    src/renderer_probe.cpp
    src/scene_probe.cpp
    src/mod.cpp)

target_include_directories(h47vr_core PUBLIC include)

target_link_libraries(h47vr_core
    PUBLIC h47vr_pins_headers
    PRIVATE bcrypt h47vr_minhook)

if(MSVC)
    target_compile_options(h47vr_core PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
