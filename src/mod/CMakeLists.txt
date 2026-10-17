# Standalone mod module. The opengl32 proxy loads the same code path by itself;
# this DLL exists so the diagnostic launcher can inject the mod into an
# already-running or deliberately launched game.
add_library(h47vr_mod SHARED
    dllmain.cpp)

target_link_libraries(h47vr_mod PRIVATE h47vr_core)

set_target_properties(h47vr_mod PROPERTIES
    OUTPUT_NAME h47vr_mod
    PREFIX "")

if(MSVC)
    target_compile_options(h47vr_mod PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
