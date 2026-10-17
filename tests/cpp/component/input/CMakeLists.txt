add_executable(inputnext_input_component_tests latest_value_tests.cpp)
target_link_libraries(inputnext_input_component_tests PRIVATE inputnext_input Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_input_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_input_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 15
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
