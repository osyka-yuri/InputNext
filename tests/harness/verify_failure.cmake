if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED RESULT_ROOT)
    message(FATAL_ERROR "TEST_EXECUTABLE and RESULT_ROOT are required.")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(work_dir "${RESULT_ROOT}/failure-${run_id}")
file(MAKE_DIRECTORY "${work_dir}")

execute_process(
    COMMAND "${TEST_EXECUTABLE}" harness.assertion_failure_probe
    WORKING_DIRECTORY "${work_dir}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 10
)
file(WRITE "${work_dir}/output.txt" "${output}${errors}")

# Catch2 uses 42 for assertion failures, distinct from startup errors.
if(NOT "${result}" STREQUAL "42")
    message(FATAL_ERROR "Expected assertion failure exit code 42, got '${result}'.\n${output}${errors}")
endif()
if(NOT EXISTS "${work_dir}/entered.txt" OR EXISTS "${work_dir}/fixture.tmp")
    message(FATAL_ERROR "The failure probe did not run or its fixture resource was not released.")
endif()
if(NOT output MATCHES "Intentional assertion failure for the runner self-check")
    message(FATAL_ERROR "The probe failed before reaching its intended assertion.\n${output}${errors}")
endif()
