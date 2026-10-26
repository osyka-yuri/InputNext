add_library(inputnext_gameinput_probe_support STATIC
    supervisor.cpp
    executable_path.cpp
)
target_include_directories(inputnext_gameinput_probe_support PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_gameinput_probe_support)

if(NOT INPUTNEXT_BUILD_DIAGNOSTICS)
    return()
endif()

add_executable(inputnext_gameinput_probe
    main.cpp
    worker.cpp
    rumble_worker.cpp
    rumble_session.cpp
    rumble_window.cpp
    rumble_sequence.cpp
    rumble_evidence.cpp
    rumble_report.cpp
    dualsense_usb_worker.cpp
    dualsense_usb_candidate_selector.cpp
    dualsense_usb_context.cpp
    dualsense_usb_sequence.cpp
    dualsense_usb_evidence.cpp
    dualsense_usb_report.cpp
    hd_audio_sequence.cpp
    hd_audio_candidate.cpp
    hd_audio_evidence.cpp
    hd_audio_report.cpp
    hd_audio_step_runner.cpp
    hd_audio_worker.cpp
    report.cpp
    history_observer.cpp
    history_report.cpp
    xinput_observer.cpp
    xinput_report.cpp
    xinput_event_observer.cpp
    xinput_event_report.cpp
)
target_include_directories(inputnext_gameinput_probe PRIVATE "${PROJECT_SOURCE_DIR}/src")
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
