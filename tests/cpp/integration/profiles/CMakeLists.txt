add_executable(inputnext_profiles_loader_tests loader_tests.cpp)
target_include_directories(inputnext_profiles_loader_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_profiles_loader_tests
    PRIVATE inputnext_profiles Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_profiles_loader_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_profiles_loader_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
