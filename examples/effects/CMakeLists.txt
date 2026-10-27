add_executable(inputnext_effects_example effects_example.cpp effects_demo.c)
target_link_libraries(inputnext_effects_example PRIVATE inputnext_effects_sdk)
target_compile_features(inputnext_effects_example PRIVATE c_std_17)

inputnext_configure_target(inputnext_effects_example)

if(BUILD_TESTING AND TARGET inputnext_runtime)
    add_test(NAME effects.sdk.example_query
        COMMAND inputnext_effects_example "$<TARGET_FILE:inputnext_runtime>")
    set_tests_properties(effects.sdk.example_query PROPERTIES
        LABELS "integration;effects;sdk;dll"
        TIMEOUT 20
    )
endif()
