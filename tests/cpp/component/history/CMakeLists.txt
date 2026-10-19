add_executable(inputnext_history_component_tests
    history_store_capacity_tests.cpp
    history_store_concurrency_tests.cpp
    history_store_read_tests.cpp
)
target_include_directories(inputnext_history_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/history"
    "${PROJECT_SOURCE_DIR}/tests"
)
target_link_libraries(inputnext_history_component_tests PRIVATE inputnext_history Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_history_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_history_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
