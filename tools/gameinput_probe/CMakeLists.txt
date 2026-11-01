add_library(inputnext_gameinput_probe_support STATIC
    process/executable_path.cpp
    process/supervisor.cpp
)
target_include_directories(inputnext_gameinput_probe_support
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/tools"
)
inputnext_configure_target(inputnext_gameinput_probe_support)

if(NOT INPUTNEXT_BUILD_DIAGNOSTICS)
    return()
endif()

add_executable(inputnext_gameinput_probe
    main.cpp
    report.cpp
    manual/window.cpp
    input/worker.cpp
    input/history/observer.cpp
    input/history/report.cpp
    input/xinput/observer.cpp
    input/xinput/report.cpp
    input/xinput/event_observer.cpp
    input/xinput/event_report.cpp
    output/session.cpp
    output/dualsense_usb_candidate_selector.cpp
    output/rumble/worker.cpp
    output/rumble/sequence.cpp
    output/rumble/evidence.cpp
    output/rumble/report.cpp
    output/dualsense_usb/worker.cpp
    output/dualsense_usb/context.cpp
    output/dualsense_usb/sequence.cpp
    output/dualsense_usb/evidence.cpp
    output/dualsense_usb/report.cpp
    output/hd_audio/worker.cpp
    output/hd_audio/candidate.cpp
    output/hd_audio/evidence.cpp
    output/hd_audio/report.cpp
    output/hd_audio/sequence.cpp
    output/hd_audio/step_runner.cpp
)
target_include_directories(inputnext_gameinput_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/tools"
    "${PROJECT_SOURCE_DIR}/src"
)
set_target_properties(inputnext_gameinput_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools/gameinput_probe"
)
target_link_libraries(inputnext_gameinput_probe PRIVATE
    inputnext_gameinput_runtime inputnext_gameinput_xinput inputnext_gameinput_output
    inputnext_output_authority inputnext_output_execution inputnext_focus ole32
    inputnext_gameinput_probe_support
    inputnext_audio
)
inputnext_configure_target(inputnext_gameinput_probe)
inputnext_deploy_gameinput(inputnext_gameinput_probe)
