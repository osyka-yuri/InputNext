add_executable(inputnext_focus_component_tests
    freshness_tests.cpp
    observer_tests.cpp
)
target_include_directories(inputnext_focus_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(inputnext_focus_component_tests PRIVATE inputnext_focus Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_focus_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_focus_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
