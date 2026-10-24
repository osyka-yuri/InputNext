add_executable(inputnext_profiles_application_tests
    application_assignment_tests.cpp
    application_authority_tests.cpp
    application_replacement_tests.cpp
)
target_include_directories(inputnext_profiles_application_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_profiles_application_tests PRIVATE
    inputnext_profile_application
    inputnext_xinput
    inputnext_output_authority
    Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_profiles_application_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_profiles_application_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
