# Configure opt-in runtime sanitizers for owned targets and Catch2.

function(inputnext_verify_ubsan_runtime_file directory name expected_sha256)
    set(runtime_file "${directory}/${name}")
    if(NOT EXISTS "${runtime_file}")
        message(FATAL_ERROR "The Undefined profile requires '${runtime_file}'.")
    endif()
    file(SHA256 "${runtime_file}" actual_sha256)
    if(NOT actual_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "The Undefined profile requires the pinned LLVM 23.1.3 runtime '${name}'. "
            "Review the runtime pin and exact DLL export manifest before changing toolchains."
        )
    endif()
endfunction()

function(inputnext_validate_sanitizer_configuration)
    if(NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Address"
       AND NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Undefined")
        return()
    endif()

    if(INPUTNEXT_NATIVE_PROFILE STREQUAL "Address")
        if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
            message(FATAL_ERROR
                "The Address profile requires the native Microsoft C++ compiler. "
                "Detected ${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}."
            )
        endif()

        string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" address_architecture)
        if(address_architecture STREQUAL "x64")
            set(address_runtime_name clang_rt.asan_dynamic-x86_64.dll)
        elseif(address_architecture STREQUAL "x86")
            set(address_runtime_name clang_rt.asan_dynamic-i386.dll)
        else()
            message(FATAL_ERROR
                "The Address profile supports only x64 and x86 MSVC targets. "
                "Detected architecture '${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}'."
            )
        endif()

        get_filename_component(address_compiler_path "${CMAKE_CXX_COMPILER}" REALPATH)
        get_filename_component(address_compiler_directory "${address_compiler_path}" DIRECTORY)
        set(address_runtime_path "${address_compiler_directory}/${address_runtime_name}")
        if(NOT EXISTS "${address_runtime_path}")
            message(FATAL_ERROR
                "The Address profile requires the MSVC AddressSanitizer runtime beside the "
                "resolved C++ compiler '${address_compiler_path}': ${address_runtime_path}"
            )
        endif()

        file(WRITE "${PROJECT_BINARY_DIR}/asan-runtime-path.txt" "${address_runtime_path}\n")
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        message(FATAL_ERROR
            "The Undefined profile requires clang-cl with the MSVC frontend. "
            "Detected ${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}."
        )
    endif()

    if(NOT CMAKE_CXX_COMPILER_ARCHITECTURE_ID STREQUAL "x64")
        message(FATAL_ERROR "The Undefined profile requires an x64 clang-cl target.")
    endif()
    if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL "23.1.3")
        message(FATAL_ERROR "The Undefined profile requires clang-cl 23.1.3 and its pinned runtime.")
    endif()

    set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded PARENT_SCOPE)

    execute_process(
        COMMAND "${CMAKE_CXX_COMPILER}" --print-runtime-dir
        RESULT_VARIABLE runtime_result
        OUTPUT_VARIABLE runtime_directory
        ERROR_VARIABLE runtime_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT "${runtime_result}" STREQUAL "0" OR NOT IS_DIRECTORY "${runtime_directory}")
        message(FATAL_ERROR
            "Could not locate the clang-cl UBSan runtime using --print-runtime-dir. "
            "Result: ${runtime_result}; directory: '${runtime_directory}'; error: ${runtime_error}"
        )
    endif()

    inputnext_verify_ubsan_runtime_file("${runtime_directory}"
        clang_rt.ubsan_standalone-x86_64.lib
        e0c724adce2d3b5647dd502d1e848484f7b862ba46b377bcb0a46828ac34f7f9
    )
    inputnext_verify_ubsan_runtime_file("${runtime_directory}"
        clang_rt.ubsan_standalone_cxx-x86_64.lib
        66156cb2b370fe6a4155bd3315c6787829609b5e4b3849eb908e0f37735a5692
    )

    include(CheckCXXSourceCompiles)
    include(CMakePushCheckState)
    unset(INPUTNEXT_UBSAN_CXX_RUNTIME_LINKS CACHE)
    cmake_push_check_state(RESET)
    set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fno-sanitize-recover=undefined")
    set(CMAKE_REQUIRED_LINK_DIRECTORIES "${runtime_directory}")
    set(CMAKE_REQUIRED_LINK_OPTIONS
        "LINKER:/INCLUDE:__ubsan_handle_dynamic_type_cache_miss_abort"
    )
    set(CMAKE_TRY_COMPILE_TARGET_TYPE EXECUTABLE)
    set(CMAKE_TRY_COMPILE_CONFIGURATION "${CMAKE_BUILD_TYPE}")
    check_cxx_source_compiles([=[
#include <climits>
#include <vector>

int main(int argc, char** argv)
{
    (void)argv;
    std::vector<int> values{argc};
    volatile int operand = values.front();
    volatile int overflow = operand + INT_MAX;
    return overflow == 0;
}
]=] INPUTNEXT_UBSAN_CXX_RUNTIME_LINKS)
    cmake_pop_check_state()

    if(NOT INPUTNEXT_UBSAN_CXX_RUNTIME_LINKS)
        message(FATAL_ERROR
            "The Undefined profile requires a working x64 clang-cl UBSan C++ compile/link runtime "
            "that matches the static MSVC CRT (/MT). See CMakeFiles/CMakeConfigureLog.yaml."
        )
    endif()

    set(INPUTNEXT_UBSAN_RUNTIME_DIRECTORY "${runtime_directory}" PARENT_SCOPE)
    message(STATUS "UBSan C++ runtime: ${runtime_directory} (compile/link check passed)")
endfunction()

function(inputnext_configure_sanitizer target)
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "INTERFACE_LIBRARY" OR target_type STREQUAL "UTILITY")
        return()
    endif()

    if(NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Address"
       AND NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Undefined")
        return()
    endif()

    if(target_type STREQUAL "STATIC_LIBRARY" OR target_type STREQUAL "OBJECT_LIBRARY")
        set(compile_scope PUBLIC)
        set(link_scope INTERFACE)
    else()
        set(compile_scope PRIVATE)
        set(link_scope PRIVATE)
    endif()

    if(INPUTNEXT_NATIVE_PROFILE STREQUAL "Address")
        set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_CHECKS "")
        target_compile_options(${target} ${compile_scope} /fsanitize=address)
        target_link_options(${target} ${link_scope} /INCREMENTAL:NO)
        return()
    endif()

    target_compile_options(${target} ${compile_scope}
        -fsanitize=undefined
        -fno-sanitize-recover=undefined
    )
    target_link_directories(${target} ${link_scope} "${INPUTNEXT_UBSAN_RUNTIME_DIRECTORY}")

    if(NOT target_type STREQUAL "STATIC_LIBRARY" AND NOT target_type STREQUAL "OBJECT_LIBRARY")
        # Pure C targets use the C runtime; C++ links also require vptr support.
        target_link_options(${target} PRIVATE
            "$<$<LINK_LANGUAGE:CXX>:LINKER:/INCLUDE:__ubsan_handle_dynamic_type_cache_miss_abort>"
        )
    endif()
endfunction()
