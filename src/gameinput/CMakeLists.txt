add_library(inputnext_gameinput_history_clock STATIC history/clock_mapping.cpp)
target_include_directories(inputnext_gameinput_history_clock PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_gameinput_history_clock PUBLIC inputnext_input)
inputnext_configure_target(inputnext_gameinput_history_clock)

add_library(inputnext_gameinput_capture STATIC
    history/history_capture_log.cpp
    history/history_capture.cpp
)
target_include_directories(inputnext_gameinput_capture PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_gameinput_capture PUBLIC inputnext_input)
inputnext_configure_target(inputnext_gameinput_capture)

add_library(inputnext_gameinput_history STATIC history/source_history_cursor.cpp)
target_include_directories(inputnext_gameinput_history PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_gameinput_history
    PUBLIC inputnext_gameinput_capture inputnext_input
)
inputnext_configure_target(inputnext_gameinput_history)

add_library(inputnext_gameinput_core STATIC core/publication_source.cpp)
target_include_directories(inputnext_gameinput_core PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_gameinput_core
    PUBLIC inputnext_devices
    PRIVATE inputnext_gameinput_capture
)
inputnext_configure_target(inputnext_gameinput_core)

add_subdirectory(dualsense)

if(INPUTNEXT_BUILD_GAMEINPUT)
    include("${PROJECT_SOURCE_DIR}/cmake/GameInput.cmake")
    add_library(inputnext_gameinput_native STATIC
        native/windows_gameinput.cpp
        native/windows_raw_output_report.cpp
        native/windows_history_cursor.cpp
        native/native_history_cursor.cpp
        native/gameinput_value_mapping.cpp
    )
    target_include_directories(inputnext_gameinput_native
        PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(inputnext_gameinput_native
        PRIVATE inputnext_gameinput_sdk inputnext_gameinput_history_clock
    )

    inputnext_configure_target(inputnext_gameinput_native)

    add_library(inputnext_gameinput_runtime STATIC
        runtime/admission.cpp
        runtime/lifecycle.cpp
        runtime/output_access.cpp
        runtime/native_cleanup.cpp
        runtime/metadata.cpp
        runtime/readings.cpp
        runtime/history.cpp
        runtime/history_provider.cpp
    )
    target_link_libraries(inputnext_gameinput_runtime
        PUBLIC inputnext_devices
        PRIVATE inputnext_history inputnext_gameinput_core inputnext_gameinput_history
            inputnext_gameinput_native inputnext_windows_metadata
    )
    target_include_directories(inputnext_gameinput_runtime PRIVATE "${PROJECT_SOURCE_DIR}/src")
    inputnext_configure_target(inputnext_gameinput_runtime)

    add_subdirectory(rumble)

    add_library(inputnext_gameinput_output STATIC
        output/connections/manager.cpp
        output/connections/worker.cpp
        output/connections/description.cpp
        output/connections/qualification.cpp
        output/connections/admission.cpp
        output/connections/retirement.cpp
        output/usb/qualification.cpp
    )
    target_include_directories(inputnext_gameinput_output
        PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(inputnext_gameinput_output
        PUBLIC inputnext_gameinput_runtime inputnext_gameinput_rumble
            inputnext_gameinput_dualsense_usb inputnext_output_execution
    )
    inputnext_configure_target(inputnext_gameinput_output)

    add_library(inputnext_gameinput_xinput STATIC
        presentation/gameinput_xinput.cpp
    )
    target_include_directories(inputnext_gameinput_xinput
        PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(inputnext_gameinput_xinput
        PUBLIC inputnext_gameinput_runtime inputnext_focus inputnext_xinput
    )
    inputnext_configure_target(inputnext_gameinput_xinput)
endif()
