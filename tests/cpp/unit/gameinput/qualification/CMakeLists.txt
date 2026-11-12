add_executable(inputnext_gameinput_input_qualification_tests
    qualification_tests.cpp
    usb_touch_report_tests.cpp
)
target_link_libraries(inputnext_gameinput_input_qualification_tests PRIVATE
    inputnext_gameinput_input_policy Catch2::Catch2WithMain
)
target_include_directories(inputnext_gameinput_input_qualification_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
inputnext_configure_target(inputnext_gameinput_input_qualification_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_input_qualification_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
