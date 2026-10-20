add_executable(inputnext_focus_integration_tests current_process_tests.cpp)
target_link_libraries(inputnext_focus_integration_tests PRIVATE
    inputnext_focus
    Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_focus_integration_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_focus_integration_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
