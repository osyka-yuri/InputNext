add_executable(inputnext_effects_integration_tests
    effects_service_tests.cpp
    effects_progress_tests.cpp
    effects_progress_correlation_tests.cpp
    effects_lifecycle_tests.cpp
)
target_link_libraries(inputnext_effects_integration_tests PRIVATE
    inputnext_effects
    inputnext_runtime_session
    Catch2::Catch2WithMain
)
target_include_directories(inputnext_effects_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tests/support/effects"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration/runtime/session"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration/gameinput/runtime"
)
inputnext_configure_target(inputnext_effects_integration_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_effects_integration_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 60
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
