add_executable(inputnext_metadata_component_tests provider_tests.cpp)
target_link_libraries(inputnext_metadata_component_tests PRIVATE inputnext_windows_metadata Catch2::Catch2WithMain)
target_include_directories(inputnext_metadata_component_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
inputnext_configure_target(inputnext_metadata_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_metadata_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
