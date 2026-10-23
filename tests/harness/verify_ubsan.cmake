if(NOT DEFINED TEST_EXECUTABLE OR NOT DEFINED RESULT_ROOT)
    message(FATAL_ERROR "TEST_EXECUTABLE and RESULT_ROOT are required.")
endif()

string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run_id)
set(work_dir "${RESULT_ROOT}/ubsan-${run_id}")
file(MAKE_DIRECTORY "${work_dir}")

# A recovering or uninstrumented probe reaches return 0 and must fail this check.
set(ENV{UBSAN_OPTIONS} "halt_on_error=0:abort_on_error=0:exitcode=86:print_stacktrace=0")
execute_process(
    COMMAND "${TEST_EXECUTABLE}"
    WORKING_DIRECTORY "${work_dir}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE errors
    TIMEOUT 5
)
file(WRITE "${work_dir}/output.txt" "${output}${errors}")

if(NOT "${result}" STREQUAL "86"
   OR NOT errors MATCHES "runtime error: signed integer overflow"
   OR NOT errors MATCHES "ubsan_probe\\.cpp")
    message(FATAL_ERROR
        "Expected the UBSan overflow report and exit code 86, got '${result}'.\n${output}${errors}"
    )
endif()
message(STATUS "UBSan detected signed overflow and terminated the probe; report: ${work_dir}/output.txt")
