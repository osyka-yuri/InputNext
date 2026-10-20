add_executable(inputnext_xinput_unit_tests
    query_projection_tests.cpp
    translation_tests.cpp
)
target_link_libraries(inputnext_xinput_unit_tests PRIVATE inputnext_xinput Catch2::Catch2WithMain)
target_include_directories(inputnext_xinput_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
inputnext_configure_target(inputnext_xinput_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_xinput_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
