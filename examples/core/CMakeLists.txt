add_executable(inputnext_core_example core_example.cpp core_reader.c)
target_link_libraries(inputnext_core_example PRIVATE inputnext_core_sdk)
target_compile_features(inputnext_core_example PRIVATE c_std_17)

inputnext_configure_target(inputnext_core_example)

if(BUILD_TESTING AND TARGET inputnext_runtime)
    add_test(NAME core.sdk.example_query
        COMMAND inputnext_core_example "$<TARGET_FILE:inputnext_runtime>")
    set_tests_properties(core.sdk.example_query PROPERTIES
        LABELS "integration;core;sdk;dll"
        TIMEOUT 20
    )
endif()
