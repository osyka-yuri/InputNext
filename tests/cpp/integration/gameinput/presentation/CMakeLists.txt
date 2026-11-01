add_executable(inputnext_gameinput_xinput_tests
    presentation_tests.cpp
    event_composition_tests.cpp
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/input/xinput/event_observer.cpp"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe/input/xinput/event_report.cpp"
)
target_link_libraries(inputnext_gameinput_xinput_tests PRIVATE
    inputnext_gameinput_xinput inputnext_gameinput_runtime Catch2::Catch2WithMain
)
target_include_directories(inputnext_gameinput_xinput_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tools"
)
inputnext_configure_target(inputnext_gameinput_xinput_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_xinput_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
