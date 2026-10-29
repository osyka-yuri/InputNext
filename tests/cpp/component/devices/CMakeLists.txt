add_executable(inputnext_device_component_tests
    device_registry_tests.cpp
    device_registry_concurrency_tests.cpp
    device_registry_race_tests.cpp
    device_pin_release_protocol_tests.cpp
    device_pin_validation_tests.cpp
)
target_include_directories(inputnext_device_component_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src/devices"
)
target_link_libraries(inputnext_device_component_tests PRIVATE inputnext_devices Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_device_component_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_device_component_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

if(INPUTNEXT_BUILD_STRESS_TESTS)
    add_executable(inputnext_device_stress_tests
        device_registry_stress_tests.cpp
        device_registry_lifecycle_stress_tests.cpp
    )
    target_link_libraries(inputnext_device_stress_tests PRIVATE inputnext_devices Catch2::Catch2WithMain)
    inputnext_configure_target(inputnext_device_stress_tests)
    catch_discover_tests(inputnext_device_stress_tests
        ADD_TAGS_AS_LABELS
        PROPERTIES TIMEOUT 30
        OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
        OUTPUT_SUFFIX .txt
    )
endif()
