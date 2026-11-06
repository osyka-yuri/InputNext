cmake_minimum_required(VERSION 4.4.4)

function(build_import_library output_variable)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIRECTORY}"
            --target inputnext_xinput14_import_library --config "${BUILD_CONFIGURATION}" --verbose
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE errors
        TIMEOUT 60
    )
    if(NOT "${result}" STREQUAL "0")
        message(FATAL_ERROR "Import library build failed (${result}):\n${output}\n${errors}")
    endif()
    set(${output_variable} "${output}\n${errors}" PARENT_SCOPE)
endfunction()

build_import_library(initial_output)
if(NOT EXISTS "${BUILD_DIRECTORY}/lib/xinput14-import.lib")
    message(FATAL_ERROR "The import library target did not produce its declared library.")
endif()
build_import_library(repeated_output)
if(repeated_output MATCHES "/[dD][eE][fF]:")
    message(FATAL_ERROR "An unchanged build reran the import librarian:\n${repeated_output}")
endif()
