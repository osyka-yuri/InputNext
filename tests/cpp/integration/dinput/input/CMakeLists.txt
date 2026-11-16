add_executable(inputnext_dinput_input_tests
    catalogue_native_fakes.cpp
    immediate_tests.cpp
    buffered_tests.cpp
    buffered_capacity_tests.cpp
    buffered_acquisition_tests.cpp
    buffered_lifecycle_tests.cpp
    buffered_handoff_tests.cpp
    device_info_tests.cpp
    focus_tests.cpp
    lifecycle_tests.cpp
    catalogue_tests.cpp
    discovery_tests.cpp
    exclusive_catalogue_tests.cpp
    exclusive_policy_tests.cpp
    exclusive_wait_tests.cpp
    exclusive_device_tests.cpp
    exclusive_native_tests.cpp
    exclusive_background_tests.cpp
    background_exclusive_tests.cpp
)
target_link_libraries(inputnext_dinput_input_tests PRIVATE
    inputnext_dinput inputnext_core Catch2::Catch2WithMain dinput8
)
target_include_directories(inputnext_dinput_input_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_dinput_input_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_dinput_input_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 45
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
