add_executable(inputnext_xinput_component_tests
    events_tests.cpp
    retrieval_tests.cpp
    guide_wait_tests.cpp
    service_packet_tests.cpp
    service_assignment_tests.cpp
    service_focus_tests.cpp
    service_query_tests.cpp
    get_state_read_tests.cpp
    get_state_preparation_tests.cpp
    get_state_lifecycle_tests.cpp
    get_state_pin_tests.cpp
    connection_status_tests.cpp
    output_tests.cpp
    output_transaction_tests.cpp
)
target_link_libraries(inputnext_xinput_component_tests PRIVATE
    inputnext_xinput Catch2::Catch2WithMain
)
target_include_directories(inputnext_xinput_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
inputnext_configure_target(inputnext_xinput_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_xinput_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 30
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
