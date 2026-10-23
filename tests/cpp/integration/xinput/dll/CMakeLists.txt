function(inputnext_add_runtime_fixture target source definition directory)
    add_library(${target} SHARED "${source}" "${definition}")
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/include"
    )
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME inputnext_runtime
        PREFIX ""
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${directory}"
        ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${directory}"
        PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/${directory}"
    )
    inputnext_configure_target(${target})
endfunction()

set(abi_expected_directory "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${abi_expected_directory}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/abi/xinput1_3.json"
    "${PROJECT_SOURCE_DIR}/abi/xinput1_4.json"
    "${PROJECT_SOURCE_DIR}/abi/xinput9_1_0.json"
    "${PROJECT_SOURCE_DIR}/abi/inputnext_runtime.json"
)
include("${CMAKE_CURRENT_SOURCE_DIR}/abi_test_oracle.cmake")
inputnext_generate_abi_expected_header(
    "${abi_expected_directory}/abi_expected_exports.hpp")

add_library(inputnext_xinput_external_callback_fixture SHARED
    external_callback_fixture.cpp
    external_callback_fixture.def
)
target_include_directories(inputnext_xinput_external_callback_fixture PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include"
)
set_target_properties(inputnext_xinput_external_callback_fixture PROPERTIES
    OUTPUT_NAME inputnext_external_callback
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/external-callback"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/external-callback"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/fixtures/external-callback"
)
inputnext_configure_target(inputnext_xinput_external_callback_fixture)

inputnext_add_runtime_fixture(inputnext_xinput_table_fixture
    table_runtime_fixture.cpp table_runtime_fixture.def table)
target_link_libraries(inputnext_xinput_table_fixture PRIVATE
    inputnext_xinput_external_callback_fixture)
function(inputnext_add_malformed_runtime_fixture target directory mode)
    inputnext_add_runtime_fixture(${target}
        table_runtime_fixture.cpp table_runtime_fixture.def ${directory})
    target_compile_definitions(${target} PRIVATE
        INPUTNEXT_XINPUT_FIXTURE_DEFAULT_QUERY_MODE=${mode})
    target_link_libraries(${target} PRIVATE inputnext_xinput_external_callback_fixture)
endfunction()
inputnext_add_malformed_runtime_fixture(inputnext_xinput_invalid_size_fixture invalid-size 1)
inputnext_add_malformed_runtime_fixture(inputnext_xinput_invalid_version_fixture invalid-version 2)
inputnext_add_malformed_runtime_fixture(inputnext_xinput_missing_callback_fixture
    missing-callback 3)
inputnext_add_malformed_runtime_fixture(inputnext_xinput_null_table_fixture null-table 4)
inputnext_add_runtime_fixture(inputnext_xinput_missing_query_fixture
    table_runtime_fixture.cpp missing_query_runtime_fixture.def missing-query)
target_link_libraries(inputnext_xinput_missing_query_fixture PRIVATE
    inputnext_xinput_external_callback_fixture)
inputnext_add_runtime_fixture(inputnext_xinput_ar1_fixture
    ar1_runtime_fixture.cpp ar1_runtime_fixture.def rejected-pin)
target_link_libraries(inputnext_xinput_ar1_fixture PRIVATE inputnext_runtime_query_front)

add_executable(inputnext_xinput_dll_probe
    dll_probe_argument_checks.cpp
    dll_probe_companion.cpp
    dll_probe_companion_boundaries.cpp
    dll_probe_imports.cpp
    dll_probe_child.cpp
    dll_probe_forwarding.cpp
    dll_probe_production.cpp
    dll_probe_provider_addresses.cpp
    dll_probe_resolver.cpp
    dll_probe_support.cpp
)
target_include_directories(inputnext_xinput_dll_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
inputnext_configure_target(inputnext_xinput_dll_probe)
target_link_libraries(inputnext_xinput_dll_probe PRIVATE dbghelp)

add_executable(inputnext_xinput_import_probe xinput_import_probe.cpp)
target_link_libraries(inputnext_xinput_import_probe PRIVATE xinput)
add_dependencies(inputnext_xinput_import_probe inputnext_xinput14)
target_include_directories(inputnext_xinput_import_probe PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
set_target_properties(inputnext_xinput_import_probe PROPERTIES
    OUTPUT_NAME inputnext_xinput_import_probe
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/import-probe"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/import-probe"
)
inputnext_configure_target(inputnext_xinput_import_probe)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(import13_definition "${CMAKE_CURRENT_SOURCE_DIR}/xinput13_import_x86.def")
else()
    set(import13_definition "${CMAKE_CURRENT_SOURCE_DIR}/xinput13_import.def")
endif()
# The fixture imports the public GetState ordinal with the caller's native calling convention.
set(import13_library "${abi_expected_directory}/xinput13-import.lib")
add_custom_command(OUTPUT "${import13_library}"
    BYPRODUCTS "${abi_expected_directory}/xinput13-import.exp"
    COMMAND "${CMAKE_AR}" /nologo "/machine:${inputnext_compiler_architecture}"
        "/def:${import13_definition}" "/out:${import13_library}"
    DEPENDS "${import13_definition}"
    VERBATIM
)
add_executable(inputnext_xinput13_import_probe xinput_import_probe.cpp "${import13_library}")
target_link_libraries(inputnext_xinput13_import_probe PRIVATE "${import13_library}")
target_compile_definitions(inputnext_xinput13_import_probe PRIVATE INPUTNEXT_XINPUT_IMPORT13=1)
target_include_directories(inputnext_xinput13_import_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
set_target_properties(inputnext_xinput13_import_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/import-probe"
    PDB_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/import-probe"
)
inputnext_configure_target(inputnext_xinput13_import_probe)
add_dependencies(inputnext_xinput13_import_probe inputnext_xinput13)

set(manual_native_directory "${CMAKE_CURRENT_BINARY_DIR}/manual-native")
if(CMAKE_CONFIGURATION_TYPES)
    string(APPEND manual_native_directory "/$<CONFIG>")
endif()
add_custom_target(inputnext_xinput_manual_native_stage
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "${manual_native_directory}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_xinput13>"
        "${manual_native_directory}/xinput1_3.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_xinput14>"
        "${manual_native_directory}/xinput1_4.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_xinput910>"
        "${manual_native_directory}/xinput9_1_0.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_runtime>"
        "${manual_native_directory}/inputnext_runtime.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE_DIR:inputnext_runtime>/GameInputRedist.dll"
        "${manual_native_directory}/GameInputRedist.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE_DIR:inputnext_runtime>/GameInputRawInputProxy.exe"
        "${manual_native_directory}/GameInputRawInputProxy.exe"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:inputnext_xinput_import_probe>"
        "${manual_native_directory}/inputnext_xinput_import_probe.exe"
    VERBATIM
)
add_dependencies(inputnext_xinput_manual_native_stage
    inputnext_xinput_bundle inputnext_xinput_import_probe)

add_executable(inputnext_xinput_dll_tests
    dll_export_tests.cpp
    dll_tests.cpp
    dll_test_support.cpp
    native_prerequisite_tests.cpp
    pe_exports.cpp
    pe_export_policy.cpp
)
target_link_libraries(inputnext_xinput_dll_tests PRIVATE
    inputnext_gameinput_probe_support Catch2::Catch2WithMain
)
target_include_directories(inputnext_xinput_dll_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/tests/support"
    "${PROJECT_SOURCE_DIR}/tools/gameinput_probe"
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${abi_expected_directory}"
)
target_compile_definitions(inputnext_xinput_dll_tests PRIVATE
    INPUTNEXT_XINPUT_BUNDLE_DIRECTORY="$<TARGET_FILE_DIR:inputnext_runtime>"
    INPUTNEXT_XINPUT_TABLE_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_table_fixture>"
    INPUTNEXT_XINPUT_EXTERNAL_CALLBACK_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_external_callback_fixture>"
    INPUTNEXT_XINPUT_INVALID_SIZE_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_invalid_size_fixture>"
    INPUTNEXT_XINPUT_INVALID_VERSION_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_invalid_version_fixture>"
    INPUTNEXT_XINPUT_MISSING_CALLBACK_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_missing_callback_fixture>"
    INPUTNEXT_XINPUT_NULL_TABLE_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_null_table_fixture>"
    INPUTNEXT_XINPUT_MISSING_QUERY_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_missing_query_fixture>"
    INPUTNEXT_XINPUT_AR1_FIXTURE_PATH="$<TARGET_FILE:inputnext_xinput_ar1_fixture>"
    INPUTNEXT_XINPUT_IMPORT_PROBE_PATH="$<TARGET_FILE:inputnext_xinput_import_probe>"
    INPUTNEXT_XINPUT13_IMPORT_PROBE_PATH="$<TARGET_FILE:inputnext_xinput13_import_probe>"
)
if(INPUTNEXT_NATIVE_PROFILE STREQUAL "Undefined")
    target_compile_definitions(inputnext_xinput_dll_tests PRIVATE INPUTNEXT_TEST_UBSAN_EXPORTS=1)
endif()
inputnext_configure_target(inputnext_xinput_dll_tests)
add_dependencies(inputnext_xinput_dll_tests
    inputnext_xinput_bundle inputnext_xinput_dll_probe
    inputnext_xinput_external_callback_fixture inputnext_xinput_table_fixture
    inputnext_xinput_invalid_size_fixture
    inputnext_xinput_invalid_version_fixture inputnext_xinput_missing_callback_fixture
    inputnext_xinput_null_table_fixture inputnext_xinput_missing_query_fixture
    inputnext_xinput_ar1_fixture inputnext_xinput_import_probe inputnext_xinput13_import_probe
)

add_test(NAME xinput.dll.abi_manifest_malformed_cases
    COMMAND "${CMAKE_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_malformed_abi_manifests.cmake")
set_tests_properties(xinput.dll.abi_manifest_malformed_cases PROPERTIES
    LABELS "integration;xinput;dll;abi"
    TIMEOUT 30
)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_xinput_dll_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 75
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
