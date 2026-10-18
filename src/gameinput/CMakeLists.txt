add_library(inputnext_gameinput_core STATIC core/publication_source.cpp)
target_include_directories(inputnext_gameinput_core PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_gameinput_core PUBLIC inputnext_devices)
inputnext_configure_target(inputnext_gameinput_core)

if(INPUTNEXT_BUILD_GAMEINPUT)
    include("${PROJECT_SOURCE_DIR}/cmake/GameInput.cmake")
    add_library(inputnext_gameinput_native STATIC
        native/windows_gameinput.cpp
        native/gameinput_value_mapping.cpp
    )
    target_include_directories(inputnext_gameinput_native
        PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(inputnext_gameinput_native PRIVATE inputnext_gameinput_sdk)

    inputnext_configure_target(inputnext_gameinput_native)

    add_library(inputnext_gameinput_runtime STATIC
        runtime/admission.cpp
        runtime/lifecycle.cpp
        runtime/metadata.cpp
        runtime/readings.cpp
    )
    target_link_libraries(inputnext_gameinput_runtime
        PUBLIC inputnext_devices
        PRIVATE inputnext_gameinput_core inputnext_gameinput_native inputnext_windows_metadata
    )
    target_include_directories(inputnext_gameinput_runtime PRIVATE "${PROJECT_SOURCE_DIR}/src")
    inputnext_configure_target(inputnext_gameinput_runtime)
endif()
