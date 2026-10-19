add_executable(inputnext_publication_integration_tests publication_source_tests.cpp)
target_link_libraries(inputnext_publication_integration_tests PRIVATE inputnext_gameinput_core Catch2::Catch2WithMain)
target_include_directories(inputnext_publication_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
)
inputnext_configure_target(inputnext_publication_integration_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_publication_integration_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
