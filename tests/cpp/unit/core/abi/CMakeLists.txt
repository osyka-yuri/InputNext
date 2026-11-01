add_executable(inputnext_core_header_c core_header_c.c)
target_link_libraries(inputnext_core_header_c PRIVATE inputnext_core_sdk)
inputnext_configure_c_target(inputnext_core_header_c)

add_executable(inputnext_core_header_cpp core_header_cpp.cpp)
target_link_libraries(inputnext_core_header_cpp PRIVATE inputnext_core_sdk)
inputnext_configure_target(inputnext_core_header_cpp)

add_executable(inputnext_core_projection_tests
    core_projection_tests.cpp
    "${PROJECT_SOURCE_DIR}/src/runtime/abi/core/projection.cpp"
)
target_include_directories(inputnext_core_projection_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_core_projection_tests PRIVATE
    inputnext_core_sdk Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_core_projection_tests)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_core_projection_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 10
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)

add_test(NAME core.abi.c_header COMMAND inputnext_core_header_c)
add_test(NAME core.abi.cpp_header COMMAND inputnext_core_header_cpp)
set_tests_properties(core.abi.c_header core.abi.cpp_header PROPERTIES
    LABELS "unit;core;abi"
    TIMEOUT 10
)
