add_executable(inputnext_effects_identity_tests
    identity_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/effects/identity.cpp"
)
target_include_directories(inputnext_effects_identity_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_effects_identity_tests PRIVATE Catch2::Catch2WithMain)
inputnext_configure_target(inputnext_effects_identity_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_effects_identity_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
