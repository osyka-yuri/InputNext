set(abi_expected_directory "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${abi_expected_directory}")
include("${PROJECT_SOURCE_DIR}/tests/support/abi/manifest.cmake")
inputnext_generate_abi_expected_header(
    "${abi_expected_directory}/abi_expected_exports.hpp")

add_executable(inputnext_dinput_dll_tests
    dll_tests.cpp
    "${PROJECT_SOURCE_DIR}/tests/support/abi/pe_exports.cpp"
    "${PROJECT_SOURCE_DIR}/tests/support/abi/pe_export_policy.cpp"
)
target_include_directories(inputnext_dinput_dll_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${abi_expected_directory}"
)
target_link_libraries(inputnext_dinput_dll_tests PRIVATE Catch2::Catch2WithMain dxguid uuid)
inputnext_configure_target(inputnext_dinput_dll_tests)
add_dependencies(inputnext_dinput_dll_tests inputnext_dinput8 inputnext_runtime)

set(dinput_probe_directory "${CMAKE_CURRENT_BINARY_DIR}/probe")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND dinput_probe_directory "/$<CONFIG>")
endif()
FetchContent_GetProperties(microsoft_gameinput SOURCE_DIR gameinput_source)
add_custom_command(TARGET inputnext_dinput_dll_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dinput_probe_directory}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_dinput8>" "${dinput_probe_directory}/dinput8.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_runtime>" "${dinput_probe_directory}/inputnext_runtime.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${gameinput_source}/native/bin/${inputnext_gameinput_architecture}/GameInputRedist.dll"
        "${dinput_probe_directory}/GameInputRedist.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${gameinput_source}/native/bin/${inputnext_gameinput_architecture}/GameInputRawInputProxy.exe"
        "${dinput_probe_directory}/GameInputRawInputProxy.exe"
    VERBATIM
)
target_compile_definitions(inputnext_dinput_dll_tests PRIVATE
    INPUTNEXT_DINPUT_PROBE_DIRECTORY="${dinput_probe_directory}"
)
if(INPUTNEXT_NATIVE_PROFILE STREQUAL "Undefined")
    target_compile_definitions(inputnext_dinput_dll_tests PRIVATE INPUTNEXT_TEST_UBSAN_EXPORTS=1)
endif()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_dinput_dll_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 45
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
