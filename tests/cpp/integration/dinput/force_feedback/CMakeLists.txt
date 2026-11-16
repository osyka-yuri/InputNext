add_executable(inputnext_dinput_feedback_tests
    backend_tests.cpp
    com_tests.cpp
    controls_tests.cpp
    lifecycle_tests.cpp
    support.cpp
    ../input/catalogue_native_fakes.cpp
)

target_link_libraries(inputnext_dinput_feedback_tests PRIVATE
    inputnext_dinput inputnext_core Catch2::Catch2WithMain dinput8
)
target_include_directories(inputnext_dinput_feedback_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration/dinput/input"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_dinput_feedback_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_dinput_feedback_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 45
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
