add_executable(inputnext_output_integration_tests
    execution_input_progress_tests.cpp
    input_progress_tests.cpp
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
