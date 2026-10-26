add_executable(inputnext_audio_windows_tests windows_port_tests.cpp)
target_include_directories(inputnext_audio_windows_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(inputnext_audio_windows_tests PRIVATE
    inputnext_audio_native Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_audio_windows_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_audio_windows_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_audio_service_tests
    audio_service_tests.cpp
    audio_service_submission_tests.cpp
    audio_service_residency_tests.cpp
    audio_service_qualification_tests.cpp
    audio_service_followup_tests.cpp
)
target_include_directories(inputnext_audio_service_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_audio_service_tests PRIVATE
    inputnext_audio inputnext_gameinput_xinput Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_audio_service_tests)
catch_discover_tests(inputnext_audio_service_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
