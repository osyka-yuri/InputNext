add_executable(inputnext_gameinput_history_tests
    capture_tests.cpp
    source_history_store_tests.cpp
    source_history_native_tests.cpp
    source_history_fallback_tests.cpp
)
target_include_directories(inputnext_gameinput_history_tests
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests"
        "${PROJECT_SOURCE_DIR}/tools"
)
target_link_libraries(inputnext_gameinput_history_tests
    PRIVATE inputnext_gameinput_history inputnext_history Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_gameinput_history_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_gameinput_history_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
