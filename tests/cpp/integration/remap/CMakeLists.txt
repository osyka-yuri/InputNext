add_executable(inputnext_remap_integration_tests remap_tests.cpp provider_switch_tests.cpp)
target_link_libraries(inputnext_remap_integration_tests PRIVATE
    inputnext_gameinput_core inputnext_gameinput_history inputnext_history inputnext_xinput
    Catch2::Catch2WithMain
)
target_include_directories(inputnext_remap_integration_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
)
inputnext_configure_target(inputnext_remap_integration_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_remap_integration_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
