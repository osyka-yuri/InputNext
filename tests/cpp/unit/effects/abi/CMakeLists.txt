add_library(inputnext_effects_header_c OBJECT effects_header_c.c)
target_compile_features(inputnext_effects_header_c PRIVATE c_std_17)
target_link_libraries(inputnext_effects_header_c PRIVATE inputnext_effects_sdk)
inputnext_configure_c_target(inputnext_effects_header_c)

add_library(inputnext_effects_header_cpp OBJECT effects_header_cpp.cpp)
target_compile_features(inputnext_effects_header_cpp PRIVATE cxx_std_23)
target_link_libraries(inputnext_effects_header_cpp PRIVATE inputnext_effects_sdk)
inputnext_configure_target(inputnext_effects_header_cpp)

add_executable(inputnext_effects_projection_tests
    effects_projection_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/runtime/abi/core_api_projection.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/abi/effects_projection.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/abi/effects_projection_body.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/abi/effects_projection_discrete.cpp"
    "${PROJECT_SOURCE_DIR}/src/runtime/abi/effects_projection_progress.cpp"
)
target_include_directories(inputnext_effects_projection_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
)
target_link_libraries(inputnext_effects_projection_tests PRIVATE
    inputnext_effects_sdk
    inputnext_core_sdk
    inputnext_output_policy
    inputnext_output_pcm
    Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_effects_projection_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_effects_projection_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
