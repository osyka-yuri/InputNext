add_executable(inputnext_gameinput_native_worker native_worker.cpp)
target_link_libraries(inputnext_gameinput_native_worker PRIVATE inputnext_gameinput_native)
target_include_directories(inputnext_gameinput_native_worker PRIVATE "${PROJECT_SOURCE_DIR}/src")
inputnext_configure_target(inputnext_gameinput_native_worker)
inputnext_deploy_gameinput(inputnext_gameinput_native_worker)

add_executable(inputnext_gameinput_native_tests callback_drain_tests.cpp)
target_include_directories(inputnext_gameinput_native_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/tools"
)
target_link_libraries(inputnext_gameinput_native_tests PRIVATE
    inputnext_gameinput_probe_support Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_gameinput_native_tests)
add_dependencies(inputnext_gameinput_native_tests inputnext_gameinput_native_worker)
# Run the physical callback-drain case explicitly with [hardware].

catch_discover_tests(inputnext_gameinput_native_tests
    TEST_SPEC "~[hardware]"
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 15
)
