add_executable(inputnext_input_unit_tests
    gamepad_state_tests.cpp
    program_tests.cpp
    touch_resolver_tests.cpp
)
target_link_libraries(inputnext_input_unit_tests PRIVATE inputnext_input Catch2::Catch2WithMain)
target_include_directories(inputnext_input_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_input_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_input_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 10
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
