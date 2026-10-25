add_executable(inputnext_output_policy_unit_tests
    body_policy_tests.cpp
    policy_tests.cpp
)
target_link_libraries(inputnext_output_policy_unit_tests
    PRIVATE inputnext_output_policy Catch2::Catch2WithMain
)
target_include_directories(inputnext_output_policy_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
inputnext_configure_target(inputnext_output_policy_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_output_policy_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_output_qualification_unit_tests
    body_projection_tests.cpp
    execution_projection_tests.cpp
    qualification_tests.cpp
)
target_include_directories(inputnext_output_qualification_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_output_qualification_unit_tests PRIVATE
    inputnext_output_authority Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_qualification_unit_tests)

catch_discover_tests(inputnext_output_qualification_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_output_drive_policy_unit_tests execution_drive_policy_tests.cpp)
target_include_directories(inputnext_output_drive_policy_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_output_drive_policy_unit_tests PRIVATE
    inputnext_output_execution Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_drive_policy_unit_tests)

catch_discover_tests(inputnext_output_drive_policy_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_output_test_support_unit_tests controlled_native_sink_tests.cpp)
target_include_directories(inputnext_output_test_support_unit_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_output_test_support_unit_tests PRIVATE
    inputnext_output_policy Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_test_support_unit_tests)

catch_discover_tests(inputnext_output_test_support_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_body_mixer_unit_tests body_mixer_tests.cpp)
target_include_directories(inputnext_body_mixer_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(inputnext_body_mixer_unit_tests PRIVATE
    inputnext_body_playback Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_body_mixer_unit_tests)

catch_discover_tests(inputnext_body_mixer_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
