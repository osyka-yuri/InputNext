add_executable(inputnext_core_tests
    catalogue_tests.cpp
    latest_tests.cpp
    reader_tests.cpp
    reader_focus_tests.cpp
    reader_lifecycle_tests.cpp
    reader_open_tests.cpp
)
target_link_libraries(inputnext_core_tests PRIVATE inputnext_core Catch2::Catch2WithMain)
target_include_directories(inputnext_core_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/cpp/integration"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_core_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_core_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
