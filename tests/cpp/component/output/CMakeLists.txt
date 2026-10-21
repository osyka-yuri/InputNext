add_executable(inputnext_output_component_tests
    arbitration_tests.cpp
    authority_identity_tests.cpp
    authority_lifecycle_tests.cpp
    authority_tests.cpp
    concurrency_tests.cpp
    authority_focus_tests.cpp
    internal_producer_tests.cpp
    authority_timing_tests.cpp
)
target_include_directories(inputnext_output_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_output_component_tests PRIVATE
    inputnext_output_authority Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_output_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
