# The app-local opengl32.dll. Because the game directory precedes the system
# directories in the default search order, this module is loaded in place of
# the system renderer and every export it provides is forwarded on.
add_library(h47vr_opengl32 SHARED
    dllmain.cpp
    gl_probe.cpp
    proxy.cpp
    opengl32_exports.generated.cpp
    opengl32.def)

target_include_directories(h47vr_opengl32 PRIVATE include)
target_link_libraries(h47vr_opengl32 PRIVATE h47vr_core)

set_target_properties(h47vr_opengl32 PROPERTIES
    OUTPUT_NAME opengl32
    PREFIX "")

if(MSVC)
    target_compile_options(h47vr_opengl32 PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
