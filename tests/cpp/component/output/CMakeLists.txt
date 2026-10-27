add_executable(inputnext_output_component_tests
    arbitration_tests.cpp
    authority_identity_tests.cpp
    authority_lifecycle_tests.cpp
    authority_tests.cpp
    body_audio_route_tests.cpp
    body_authority_tests.cpp
    body_capacity_tests.cpp
    body_lifecycle_tests.cpp
    body_playback_authority_tests.cpp
    body_route_qualification_tests.cpp
    concurrency_tests.cpp
    execution_owner_tests.cpp
    execution_qualification_tests.cpp
    authority_focus_tests.cpp
    internal_producer_tests.cpp
    authority_timing_tests.cpp
    xinput_rumble_tests.cpp
)
target_include_directories(inputnext_output_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_output_component_tests PRIVATE
    inputnext_output_authority Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_component_tests)

add_executable(inputnext_body_pcm_component_tests
    pcm_accounting_tests.cpp
    pcm_lifetime_tests.cpp
    pcm_validation_tests.cpp
)
target_include_directories(inputnext_body_pcm_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_body_pcm_component_tests PRIVATE
    inputnext_output_pcm Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_body_pcm_component_tests)

add_executable(inputnext_body_playback_component_tests playback_bank_tests.cpp)
target_include_directories(inputnext_body_playback_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_body_playback_component_tests PRIVATE
    inputnext_body_playback Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_body_playback_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_body_playback_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

catch_discover_tests(inputnext_body_pcm_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

catch_discover_tests(inputnext_output_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_executable(inputnext_output_execution_component_tests
    finite_submission_tests.cpp
    native_observation_tests.cpp
    body_route_registration_tests.cpp
    execution_engine_backoff_qualification_tests.cpp
    execution_engine_backoff_tests.cpp
    execution_engine_capacity_tests.cpp
    execution_engine_drive_tests.cpp
    execution_engine_registration_tests.cpp
    execution_engine_refresh_tests.cpp
    execution_engine_retirement_tests.cpp
    execution_engine_retry_tests.cpp
    execution_engine_wait_tests.cpp
)
target_include_directories(inputnext_output_execution_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_output_execution_component_tests PRIVATE
    inputnext_output_execution Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_execution_component_tests)

catch_discover_tests(inputnext_output_execution_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

inputnext_add_output_executor(inputnext_output_executor_test_runtime
    "${CMAKE_CURRENT_SOURCE_DIR}/thread_start_control.cpp"
)
target_compile_definitions(inputnext_output_executor_test_runtime
    PRIVATE INPUTNEXT_OUTPUT_TESTING=1
)

add_executable(inputnext_output_executor_component_tests
    executor_delivery_tests.cpp
    executor_lifecycle_tests.cpp
    executor_registration_tests.cpp
)
target_include_directories(inputnext_output_executor_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_compile_definitions(inputnext_output_executor_component_tests
    PRIVATE INPUTNEXT_OUTPUT_TESTING=1
)
target_link_libraries(inputnext_output_executor_component_tests PRIVATE
    inputnext_output_executor_test_runtime Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_output_executor_component_tests)

catch_discover_tests(inputnext_output_executor_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
