add_executable(inputnext_output_component_tests
    authority/arbitration_tests.cpp
    authority/authority_identity_tests.cpp
    authority/authority_lifecycle_tests.cpp
    authority/authority_tests.cpp
    authority/body_audio_route_tests.cpp
    authority/body_authority_tests.cpp
    authority/body_capacity_tests.cpp
    authority/body_lifecycle_tests.cpp
    authority/body_playback_authority_tests.cpp
    authority/body_route_qualification_tests.cpp
    authority/concurrency_tests.cpp
    authority/execution_owner_tests.cpp
    authority/execution_qualification_tests.cpp
    authority/authority_focus_tests.cpp
    authority/internal_producer_tests.cpp
    authority/authority_timing_tests.cpp
    authority/xinput_rumble_tests.cpp
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
    pcm/pcm_accounting_tests.cpp
    pcm/pcm_lifetime_tests.cpp
    pcm/pcm_validation_tests.cpp
)
target_include_directories(inputnext_body_pcm_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_body_pcm_component_tests PRIVATE
    inputnext_output_pcm Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_body_pcm_component_tests)

add_executable(inputnext_body_playback_component_tests playback/playback_bank_tests.cpp)
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
    execution/finite_submission_tests.cpp
    execution/native_observation_tests.cpp
    execution/body_route_registration_tests.cpp
    execution/execution_engine_backoff_qualification_tests.cpp
    execution/execution_engine_backoff_tests.cpp
    execution/execution_engine_capacity_tests.cpp
    execution/execution_engine_drive_tests.cpp
    execution/execution_engine_registration_tests.cpp
    execution/execution_engine_refresh_tests.cpp
    execution/execution_engine_retirement_tests.cpp
    execution/execution_engine_retry_tests.cpp
    execution/execution_engine_wait_tests.cpp
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
    "${CMAKE_CURRENT_SOURCE_DIR}/executor/thread_start_control.cpp"
)
target_compile_definitions(inputnext_output_executor_test_runtime
    PRIVATE INPUTNEXT_OUTPUT_TESTING=1
)

add_executable(inputnext_output_executor_component_tests
    executor/executor_delivery_tests.cpp
    executor/executor_lifecycle_tests.cpp
    executor/executor_registration_tests.cpp
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
