add_executable(inputnext_runtime_bootstrap_tests
    bootstrap_tests.cpp
    core_bootstrap_tests.cpp
    runtime_api_adapter_tests.cpp
    runtime_api_guard_tests.cpp
    runtime_api_test_composition.cpp
)
target_link_libraries(inputnext_runtime_bootstrap_tests PRIVATE
    inputnext_runtime_core Catch2::Catch2WithMain
)
target_include_directories(inputnext_runtime_bootstrap_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration"
)
inputnext_configure_target(inputnext_runtime_bootstrap_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_runtime_bootstrap_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
