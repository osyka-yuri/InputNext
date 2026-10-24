add_executable(inputnext_runtime_session_tests
    native_session_tests.cpp
    native_session_progress_tests.cpp
    native_session_drain_tests.cpp
    native_session_profile_tests.cpp
)
target_link_libraries(inputnext_runtime_session_tests PRIVATE
    inputnext_runtime_session inputnext_gameinput_probe_support Catch2::Catch2WithMain
)
target_include_directories(inputnext_runtime_session_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe"
)
inputnext_configure_target(inputnext_runtime_session_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_runtime_session_tests
    TEST_SPEC "~[.owned-child]"
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
