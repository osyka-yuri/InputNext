add_executable(inputnext_runtime_front_tests
    query_front_tests.cpp
    query_front_composition.cpp
)
target_link_libraries(inputnext_runtime_front_tests PRIVATE
    inputnext_runtime_core Catch2::Catch2WithMain
)
target_include_directories(inputnext_runtime_front_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
inputnext_configure_target(inputnext_runtime_front_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_runtime_front_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
