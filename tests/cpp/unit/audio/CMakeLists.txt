add_executable(inputnext_audio_unit_tests
    audio_service_policy_tests.cpp
    control_notification_tests.cpp
    native_types_tests.cpp
)
target_include_directories(inputnext_audio_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include"
)
target_link_libraries(inputnext_audio_unit_tests PRIVATE inputnext_audio Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_audio_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_audio_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_audio_windows_unit_tests
    windows_wave_format_tests.cpp
    windows_endpoint_tests.cpp
)
target_include_directories(inputnext_audio_windows_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)

target_link_libraries(inputnext_audio_windows_unit_tests PRIVATE
    inputnext_audio_native Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_audio_windows_unit_tests)
catch_discover_tests(inputnext_audio_windows_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
