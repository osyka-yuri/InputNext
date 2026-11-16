add_executable(inputnext_dinput_root_tests
    root_tests.cpp
    buffered_events_tests.cpp
    discovery_tests.cpp)
target_include_directories(inputnext_dinput_root_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(inputnext_dinput_root_tests PRIVATE
    inputnext_dinput Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_dinput_root_tests)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_dinput_root_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt)
