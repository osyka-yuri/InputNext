cmake_minimum_required(VERSION 4.4.4)

if(DEFINED CHECK_CONFIGURATION)
    set(INPUTNEXT_PRODUCTION ON CACHE BOOL "")
    set(INPUTNEXT_PGO_MODE Optimize CACHE STRING "")
    set(INPUTNEXT_PGO_DIRECTORY "${TEST_ROOT}" CACHE PATH "")
    set(CMAKE_CXX_COMPILER_ID MSVC)
    set(CMAKE_C_COMPILER_ID MSVC)
    set(CMAKE_BUILD_TYPE Release)
    set(INPUTNEXT_NATIVE_PROFILE Default)
    include("${PROJECT_ROOT}/cmake/ProductionOptions.cmake")
    inputnext_prepare_production_configuration()
    return()
endif()

function(check_profile expected_success label)
    execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROJECT_ROOT=${PROJECT_ROOT}"
        "-DTEST_ROOT=${INPUTNEXT_PGO_DIRECTORY}" -DCHECK_CONFIGURATION=ON
        -P "${CMAKE_CURRENT_FUNCTION_LIST_FILE}"
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(expected_success AND NOT result EQUAL 0)
        message(FATAL_ERROR "${label} failed: ${output}${errors}")
    elseif(NOT expected_success AND result EQUAL 0)
        message(FATAL_ERROR "${label} incorrectly passed.")
    endif()
endfunction()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run)
set(INPUTNEXT_PGO_DIRECTORY "${TEST_ROOT}/${run}" CACHE PATH "")
set(INPUTNEXT_PRODUCTION ON CACHE BOOL "")
set(INPUTNEXT_PGO_MODE Instrument CACHE STRING "")
set(CMAKE_CXX_COMPILER_ID MSVC)
set(CMAKE_C_COMPILER_ID MSVC)
set(CMAKE_BUILD_TYPE Release)
set(INPUTNEXT_NATIVE_PROFILE Default)
include("${PROJECT_ROOT}/cmake/ProductionOptions.cmake")
inputnext_prepare_production_configuration()
check_profile(FALSE "unbuilt instrumentation")
file(COPY_FILE "${INPUTNEXT_PGO_DIRECTORY}/campaign" "${INPUTNEXT_PGO_DIRECTORY}/instrumented")
file(MAKE_DIRECTORY "${INPUTNEXT_PGO_DIRECTORY}/trained")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/inputnext_runtime.pgd" "raw profile")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/trained/inputnext_runtime.pgd" "trained profile")
file(READ "${INPUTNEXT_PGO_DIRECTORY}/campaign" first_campaign)
file(SHA256 "${INPUTNEXT_PGO_DIRECTORY}/inputnext_runtime.pgd" raw_hash)
file(SHA256 "${INPUTNEXT_PGO_DIRECTORY}/trained/inputnext_runtime.pgd" trained_hash)
set(ready "{\"campaign\":\"${first_campaign}\",\"raw_sha256\":\"${raw_hash}\",\"trained_sha256\":\"${trained_hash}\"}")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/trained/ready" "${ready}")
check_profile(TRUE "completed current campaign")
file(APPEND "${INPUTNEXT_PGO_DIRECTORY}/inputnext_runtime.pgd" "changed")
check_profile(FALSE "changed raw profile")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/inputnext_runtime.pgd" "raw profile")
file(APPEND "${INPUTNEXT_PGO_DIRECTORY}/trained/inputnext_runtime.pgd" "changed")
check_profile(FALSE "changed trained profile")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/trained/inputnext_runtime.pgd" "trained profile")
check_profile(TRUE "restored current campaign")

inputnext_prepare_production_configuration()
file(READ "${INPUTNEXT_PGO_DIRECTORY}/campaign" second_campaign)
if(first_campaign STREQUAL second_campaign OR EXISTS "${INPUTNEXT_PGO_DIRECTORY}/trained/ready"
    OR EXISTS "${INPUTNEXT_PGO_DIRECTORY}/instrumented")
    message(FATAL_ERROR "New instrumentation retained the previous campaign's readiness.")
endif()
check_profile(FALSE "new campaign without link or merge")
file(COPY_FILE "${INPUTNEXT_PGO_DIRECTORY}/campaign" "${INPUTNEXT_PGO_DIRECTORY}/instrumented")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/trained/ready" "${ready}")
check_profile(FALSE "previous campaign marker restored after a new instrumented link")

file(MAKE_DIRECTORY "${INPUTNEXT_PGO_DIRECTORY}/counters")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/counters/inputnext_runtime!1.pgc" "previous campaign counters")
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/counters/other!1.pgc" "unrelated counters")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
    "-DCOUNTER_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}/counters"
    -P "${PROJECT_ROOT}/cmake/BeginPgoInstrumentation.cmake"
    COMMAND_ERROR_IS_FATAL ANY)
file(GLOB_RECURSE retired "${INPUTNEXT_PGO_DIRECTORY}/retired-counters/inputnext_runtime!1.pgc"
    "${INPUTNEXT_PGO_DIRECTORY}/retired-counters/*/inputnext_runtime!1.pgc")
list(LENGTH retired retired_count)
if(NOT retired_count EQUAL 1 OR EXISTS "${INPUTNEXT_PGO_DIRECTORY}/counters/inputnext_runtime!1.pgc"
    OR NOT EXISTS "${INPUTNEXT_PGO_DIRECTORY}/counters/other!1.pgc"
    OR EXISTS "${INPUTNEXT_PGO_DIRECTORY}/trained/ready" OR EXISTS "${INPUTNEXT_PGO_DIRECTORY}/instrumented")
    message(FATAL_ERROR "Instrumented relinking did not isolate old runtime counters and invalidate readiness.")
endif()
file(READ "${retired}" retired_contents)
if(NOT retired_contents STREQUAL "previous campaign counters")
    message(FATAL_ERROR "The previous campaign counters were not preserved.")
endif()
file(COPY_FILE "${INPUTNEXT_PGO_DIRECTORY}/campaign" "${INPUTNEXT_PGO_DIRECTORY}/instrumented")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
    "-DCOUNTER_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}/counters" "-DPGO_MANAGER=${CMAKE_COMMAND}"
    -P "${PROJECT_ROOT}/cmake/MergePgoProfiles.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR NOT errors MATCHES "No runtime training counters")
    message(FATAL_ERROR "A new campaign could merge the previous campaign's counters.")
endif()
file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/counters/inputnext_runtime!1.pgc" "controlled counters")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
    "-DCOUNTER_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}/counters" "-DPGO_MANAGER=${CMAKE_COMMAND}"
    -P "${PROJECT_ROOT}/cmake/MergePgoProfiles.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(result EQUAL 0 OR EXISTS "${INPUTNEXT_PGO_DIRECTORY}/trained/ready")
    message(FATAL_ERROR "A failed profile merge retained readiness.")
endif()
