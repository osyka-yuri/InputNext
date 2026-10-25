add_executable(inputnext_output_integration_tests
    body_playback_admission_tests.cpp
    body_route_ack_tests.cpp
    body_route_completion_tests.cpp
    body_route_failure_tests.cpp
    body_route_independence_tests.cpp
    body_route_masking_tests.cpp
    body_route_stale_completion_tests.cpp
    execution_input_progress_tests.cpp
    input_progress_tests.cpp
    pcm_input_progress_tests.cpp
    pcm_lifetime_tests.cpp
    pcm_render_qualification_tests.cpp
)
target_include_directories(inputnext_output_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_output_integration_tests PRIVATE
    inputnext_output_execution inputnext_xinput Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_integration_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_output_integration_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
