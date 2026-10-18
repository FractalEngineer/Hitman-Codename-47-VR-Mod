# Standalone graphics-bridge probe. It is the plan's first technical gate: can a
# 32-bit process hold both the legacy WGL context the game uses and an OpenXR
# session on the connected Quest?
#
# Not part of the shipped mod. It exists so the gate can be answered without
# running the game at all.

set(OPENXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/openxr")
set(OPENXR_LIB "${OPENXR_ROOT}/lib/x86/openxr_loader.lib")
set(OPENXR_DLL "${OPENXR_ROOT}/lib/x86/openxr_loader.dll")

if(NOT EXISTS "${OPENXR_LIB}")
    message(WARNING
        "h47vr: skipping the OpenXR probe - ${OPENXR_LIB} is missing.\n"
        "Run tools/fetch-openxr.ps1 to build the 32-bit loader.")
    return()
endif()

add_executable(h47vr_openxr_probe
    main.cpp)

target_include_directories(h47vr_openxr_probe PRIVATE "${OPENXR_ROOT}/include")
target_link_libraries(h47vr_openxr_probe PRIVATE "${OPENXR_LIB}" opengl32 gdi32 user32)

set_target_properties(h47vr_openxr_probe PROPERTIES
    OUTPUT_NAME h47vr-openxr-probe
    PREFIX "")

# Deliberately NOT build/bin: the app-local opengl32.dll proxy lives there, and
# Windows would load it into this process too, silently layering the mod under
# the probe. Keeping the probe in its own directory means it exercises the
# system OpenGL path, like a clean run of the game before installation.
set_target_properties(h47vr_openxr_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/probe")
foreach(_config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(h47vr_openxr_probe PROPERTIES
        "RUNTIME_OUTPUT_DIRECTORY_${_config}" "${CMAKE_BINARY_DIR}/bin/probe")
endforeach()

if(MSVC)
    target_compile_options(h47vr_openxr_probe PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()

# The loader has to sit next to the executable, exactly as it will next to the mod.
add_custom_command(TARGET h47vr_openxr_probe POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${OPENXR_DLL}" "$<TARGET_FILE_DIR:h47vr_openxr_probe>"
    COMMENT "Staging openxr_loader.dll next to the probe")
