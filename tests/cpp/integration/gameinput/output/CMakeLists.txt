add_executable(inputnext_gameinput_output_tests
    connections_tests.cpp
    usb_connections_tests.cpp
    usb_qualification_freshness_tests.cpp
    bluetooth_connections_tests.cpp
    native_progress_tests.cpp
    backend_observation_tests.cpp
    rumble_evidence_tests.cpp
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/rumble/evidence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/rumble/sequence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/output/dualsense_usb/evidence.cpp"
)
target_link_libraries(inputnext_gameinput_output_tests PRIVATE
    inputnext_gameinput_xinput inputnext_gameinput_output Catch2::Catch2WithMain
)
target_include_directories(inputnext_gameinput_output_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tools"
)
inputnext_configure_target(inputnext_gameinput_output_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_output_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
