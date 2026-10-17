add_executable(h47vr_launcher
    main.cpp)

target_link_libraries(h47vr_launcher PRIVATE h47vr_core)

set_target_properties(h47vr_launcher PROPERTIES
    OUTPUT_NAME h47vr-launcher
    PREFIX "")

if(MSVC)
    target_compile_options(h47vr_launcher PRIVATE /W4 /permissive- /EHsc /utf-8)
endif()
