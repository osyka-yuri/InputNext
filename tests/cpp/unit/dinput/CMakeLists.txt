add_executable(inputnext_dinput_unit_tests profile_tests.cpp format_tests.cpp
    effect_parameters_tests.cpp playback_tests.cpp)
target_link_libraries(inputnext_dinput_unit_tests
    PRIVATE inputnext_dinput Catch2::Catch2WithMain dinput8 dxguid
)
target_include_directories(inputnext_dinput_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_dinput_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_dinput_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
