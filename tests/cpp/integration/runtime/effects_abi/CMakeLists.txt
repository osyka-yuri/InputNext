add_executable(inputnext_effects_api_tests
    effects_api_tests.cpp
    effects_example_tests.cpp
    effects_api_pcm_overlap_tests.cpp
    "${PROJECT_SOURCE_DIR}/examples/effects/effects_demo.c"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration/runtime/bootstrap/runtime_api_test_composition.cpp"
)
target_compile_features(inputnext_effects_api_tests PRIVATE c_std_17)
target_include_directories(inputnext_effects_api_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/examples/effects"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration/runtime/bootstrap"
)
target_link_libraries(inputnext_effects_api_tests PRIVATE
    inputnext_runtime_core
    inputnext_effects_sdk
    Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_effects_api_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_effects_api_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
