if(NOT INPUTNEXT_PRODUCTION OR INPUTNEXT_PGO_MODE STREQUAL "Instrument")
    return()
endif()

set(abi_support "${PROJECT_SOURCE_DIR}/tests/support/abi")
set(generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated}")
include("${abi_support}/manifest.cmake")
inputnext_generate_abi_expected_header("${generated}/abi_expected_exports.hpp")
add_executable(inputnext_distribution_probe
    distribution_probe.cpp
    "${abi_support}/pe_exports.cpp"
    "${abi_support}/pe_export_policy.cpp"
)
target_include_directories(inputnext_distribution_probe PRIVATE
    "${PROJECT_SOURCE_DIR}/tests/support" "${generated}")

inputnext_configure_target(inputnext_distribution_probe)
add_custom_target(inputnext_distribution_checks DEPENDS
    inputnext_distribution inputnext_distribution_probe inputnext_xinput_import_probe)
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/distribution/checks.json" CONTENT
"{
  \"probe\": \"$<TARGET_FILE:inputnext_distribution_probe>\",
  \"import\": \"$<TARGET_FILE:inputnext_xinput_import_probe>\",
  \"compiler\": \"${CMAKE_CXX_COMPILER}\",
  \"ninja\": \"${CMAKE_MAKE_PROGRAM}\"
}\n")
add_test(NAME distribution.manifest_rejects_corruption
    COMMAND "${CMAKE_COMMAND}" "-DPROJECT_ROOT=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/manifest-tests"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/manifest_tests.cmake")
set_tests_properties(distribution.manifest_rejects_corruption PROPERTIES
    LABELS "integration;distribution" TIMEOUT 30)
add_test(NAME distribution.production_configuration_guards
    COMMAND "${CMAKE_COMMAND}" "-DPROJECT_ROOT=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/production-options"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/production_options_tests.cmake")
set_tests_properties(distribution.production_configuration_guards PROPERTIES
    LABELS "integration;distribution" TIMEOUT 30)
add_test(NAME distribution.pgo_campaign_readiness
    COMMAND "${CMAKE_COMMAND}" "-DPROJECT_ROOT=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/pgo-campaign"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/pgo_profile_tests.cmake")
set_tests_properties(distribution.pgo_campaign_readiness PROPERTIES
    LABELS "integration;distribution;pgo" TIMEOUT 30)
add_test(NAME distribution.metadata_refreshes_without_reconfigure
    COMMAND "${CMAKE_COMMAND}" "-DPROJECT_ROOT=${PROJECT_SOURCE_DIR}"
        "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/metadata"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/metadata_tests.cmake")
set_tests_properties(distribution.metadata_refreshes_without_reconfigure PROPERTIES
    LABELS "integration;distribution" TIMEOUT 30)
