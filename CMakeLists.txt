cmake_minimum_required(VERSION 4.4.4)

if(NOT DEFINED CMAKE_CXX_COMPILER AND NOT DEFINED ENV{CXX})
    set(CMAKE_CXX_COMPILER cl)
endif()

set(INPUTNEXT_NATIVE_PROFILE "Default" CACHE STRING "Native diagnostic profile")
set_property(CACHE INPUTNEXT_NATIVE_PROFILE PROPERTY STRINGS Default Analyze Address Undefined)
set(inputnext_native_profiles Default Analyze Address Undefined)
if(NOT INPUTNEXT_NATIVE_PROFILE IN_LIST inputnext_native_profiles)
    message(FATAL_ERROR
        "INPUTNEXT_NATIVE_PROFILE must be Default, Analyze, Address, or Undefined; "
        "got '${INPUTNEXT_NATIVE_PROFILE}'."
    )
endif()

project(InputNext VERSION 1.0.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "InputNext requires Windows and an MSVC-compatible C++ toolchain.")
endif()

string(TOLOWER "${CMAKE_CXX_COMPILER_ARCHITECTURE_ID}" inputnext_compiler_architecture)
if(NOT inputnext_compiler_architecture MATCHES "^(x64|x86)$")
    message(FATAL_ERROR "InputNext supports x64 and x86 compiler targets.")
endif()
if(DEFINED INPUTNEXT_TARGET_ARCHITECTURE
    AND NOT INPUTNEXT_TARGET_ARCHITECTURE STREQUAL inputnext_compiler_architecture)
    message(FATAL_ERROR "The compiler architecture does not match the preset. Use the matching Developer shell.")
endif()

set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(cmake/CompilerOptions.cmake)
include(cmake/NativeAnalysis.cmake)
include(cmake/Sanitizers.cmake)
inputnext_validate_native_analysis_profile()
inputnext_validate_sanitizer_configuration()

set(inputnext_gameinput_architecture "${inputnext_compiler_architecture}")
option(INPUTNEXT_BUILD_GAMEINPUT "Build the native GameInput runtime" ON)
option(INPUTNEXT_BUILD_DIAGNOSTICS "Build the interactive GameInput diagnostic" OFF)
if(INPUTNEXT_BUILD_DIAGNOSTICS AND NOT INPUTNEXT_BUILD_GAMEINPUT)
    message(FATAL_ERROR "The GameInput diagnostic requires INPUTNEXT_BUILD_GAMEINPUT=ON.")
endif()
include(CTest)

add_subdirectory(src/focus)
add_subdirectory(src/input)
add_subdirectory(src/devices)
add_subdirectory(src/output)
add_subdirectory(src/metadata)
add_subdirectory(src/history)
add_subdirectory(src/xinput)
add_subdirectory(src/gameinput)
if(INPUTNEXT_BUILD_GAMEINPUT)
    add_subdirectory(src/runtime)
    add_subdirectory(src/facade/xinput)
    if(INPUTNEXT_BUILD_DIAGNOSTICS OR BUILD_TESTING)
        add_subdirectory(tools/gameinput_probe)
    endif()
endif()
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
