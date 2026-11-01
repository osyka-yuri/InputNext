add_executable(inputnext_gameinput_probe_child_fixture probe_child_fixture.cpp)
inputnext_configure_target(inputnext_gameinput_probe_child_fixture)

add_executable(inputnext_gameinput_probe_tests
    probe_tests.cpp
    manual_window_tests.cpp
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/manual/window.cpp"
)
target_include_directories(inputnext_gameinput_probe_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tools"
)
target_link_libraries(inputnext_gameinput_probe_tests PRIVATE
    inputnext_gameinput_probe_support Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_gameinput_probe_tests)
add_dependencies(inputnext_gameinput_probe_tests
    inputnext_gameinput_probe inputnext_gameinput_probe_child_fixture
)
# The process tests locate their diagnostic and fixture beside the test executable.
set_target_properties(inputnext_gameinput_probe_tests inputnext_gameinput_probe_child_fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools/gameinput_probe"
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_probe_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 45
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
