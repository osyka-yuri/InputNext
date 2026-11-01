add_executable(inputnext_gameinput_probe_unit_tests
    report_tests.cpp
    history_report_tests.cpp
    xinput_report_tests.cpp
    xinput_event_report_tests.cpp
    rumble_sequence_tests.cpp
    manual_sequence_tests.cpp
    dualsense_usb_evidence_tests.cpp
    dualsense_usb_report_tests.cpp
    hd_audio_evidence_tests.cpp
    hd_audio_candidate_tests.cpp
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/report.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/input/history/report.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/input/xinput/report.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/input/xinput/event_report.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/rumble/sequence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/rumble/evidence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/rumble/report.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/dualsense_usb/sequence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/dualsense_usb/evidence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/dualsense_usb/report.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/hd_audio/evidence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/hd_audio/candidate.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/hd_audio/sequence.cpp"
)
target_link_libraries(inputnext_gameinput_probe_unit_tests PRIVATE Catch2::Catch2WithMain)
target_include_directories(inputnext_gameinput_probe_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tools"
)
inputnext_configure_target(inputnext_gameinput_probe_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_probe_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
