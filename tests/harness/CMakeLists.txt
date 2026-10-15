add_executable(inputnext_harness_tests runner_checks.cpp)
target_link_libraries(inputnext_harness_tests PRIVATE Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_harness_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_harness_tests
    TEST_SPEC "[harness]~[.failure-probe]"
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 10
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_test(NAME harness.failure_cleanup
    COMMAND "${CMAKE_COMMAND}"
        "-DTEST_EXECUTABLE=$<TARGET_FILE:inputnext_harness_tests>"
        "-DRESULT_ROOT=${CMAKE_CURRENT_BINARY_DIR}/results"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_failure.cmake"
)
set_tests_properties(harness.failure_cleanup PROPERTIES
    LABELS "harness;integration"
    TIMEOUT 15
)
