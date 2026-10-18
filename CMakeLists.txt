cmake_minimum_required(VERSION 3.24)

project(h47vr
    VERSION 0.1.0
    DESCRIPTION "Hitman: Codename 47 VR mod"
    LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "h47vr must be built as 32-bit x86 to load into Hitman.Exe.\n"
        "Configure with -A Win32 (Visual Studio) or the mingw i686 toolchain.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
foreach(_cfg IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_cfg} "${CMAKE_BINARY_DIR}/bin")
endforeach()

# ---------------------------------------------------------------------------
# Build pin header, generated from pins/ so the PowerShell tooling and the
# compiled mod can never disagree about the supported build.
# ---------------------------------------------------------------------------
set(H47VR_PINS_FILE "${CMAKE_CURRENT_SOURCE_DIR}/pins/build_251800.txt")
set(H47VR_PINS_HEADER "${CMAKE_CURRENT_BINARY_DIR}/generated/h47vr/pins_generated.h")

add_custom_command(
    OUTPUT "${H47VR_PINS_HEADER}"
    COMMAND "${CMAKE_COMMAND}"
            "-DH47VR_PINS=${H47VR_PINS_FILE}"
            "-DH47VR_OUT=${H47VR_PINS_HEADER}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GeneratePins.cmake"
    DEPENDS "${H47VR_PINS_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GeneratePins.cmake"
    COMMENT "Generating build pin header"
    VERBATIM)

add_custom_target(h47vr_pins DEPENDS "${H47VR_PINS_HEADER}")

add_library(h47vr_pins_headers INTERFACE)
add_dependencies(h47vr_pins_headers h47vr_pins)
target_include_directories(h47vr_pins_headers INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# ---------------------------------------------------------------------------
# Vendored dependencies
# ---------------------------------------------------------------------------
set(H47VR_MINHOOK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/minhook")
set(H47VR_MINHOOK_LIB "${H47VR_MINHOOK_ROOT}/lib/x86/minhook.lib")

if(NOT EXISTS "${H47VR_MINHOOK_LIB}")
    message(FATAL_ERROR
        "h47vr: ${H47VR_MINHOOK_LIB} is missing.\n"
        "Run tools/fetch-minhook.ps1 to build the 32-bit MinHook library.")
endif()

add_library(h47vr_minhook STATIC IMPORTED GLOBAL)
set_target_properties(h47vr_minhook PROPERTIES
    IMPORTED_LOCATION "${H47VR_MINHOOK_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${H47VR_MINHOOK_ROOT}/include")

add_subdirectory(src)
