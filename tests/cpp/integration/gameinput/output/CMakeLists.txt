add_executable(inputnext_gameinput_output_tests
    connections_tests.cpp
    rumble_evidence_tests.cpp
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/rumble_evidence.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/rumble_sequence.cpp"
)
set_property(SOURCE rumble_evidence_tests.cpp APPEND PROPERTY INCLUDE_DIRECTORIES
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe"
)
target_link_libraries(inputnext_gameinput_output_tests PRIVATE
    inputnext_gameinput_xinput inputnext_gameinput_output Catch2::Catch2WithMain
)
target_include_directories(inputnext_gameinput_output_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_gameinput_output_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_output_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
