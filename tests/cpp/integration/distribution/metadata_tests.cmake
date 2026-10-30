cmake_minimum_required(VERSION 4.4.4)

find_package(Git REQUIRED)
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef run)
set(fixture "${TEST_ROOT}/${run}")
set(PROJECT_SOURCE_DIR "${fixture}/source")
set(PROJECT_BINARY_DIR "${fixture}/build")
set(CMAKE_INSTALL_PREFIX "${fixture}/package")
file(MAKE_DIRECTORY "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}/distribution" "${CMAKE_INSTALL_PREFIX}")

function(git_fixture)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -c user.name=InputNext-Test
        -c user.email=test@example.invalid -c commit.gpgsign=false ${ARGN}
        WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY
        OUTPUT_QUIET ERROR_QUIET)
endfunction()

function(check_metadata expected_revision expected_modified)
    include("${PROJECT_BINARY_DIR}/distribution/WriteBuildInfo.cmake")
    file(READ "${CMAKE_INSTALL_PREFIX}/build-info.json" info)
    string(JSON revision GET "${info}" source_revision)
    string(JSON modified GET "${info}" source_modified)
    if(NOT revision STREQUAL expected_revision OR NOT modified STREQUAL expected_modified)
        message(FATAL_ERROR "Stale package metadata: revision=${revision}; modified=${modified}.")
    endif()
endfunction()

git_fixture(init -q)
file(WRITE "${PROJECT_SOURCE_DIR}/input.cpp" "initial source\n")
git_fixture(add input.cpp)
git_fixture(commit -qm initial)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE initial_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
file(WRITE "${PROJECT_BINARY_DIR}/distribution/build-info-base.json"
    "{\"source_revision\":\"unknown\",\"source_modified\":true}")
configure_file("${PROJECT_ROOT}/cmake/WriteBuildInfo.cmake.in"
    "${PROJECT_BINARY_DIR}/distribution/WriteBuildInfo.cmake" @ONLY)
check_metadata("${initial_revision}" OFF)

file(WRITE "${PROJECT_SOURCE_DIR}/input.cpp" "changed source\n")
check_metadata("${initial_revision}" ON)
git_fixture(add input.cpp)
git_fixture(commit -qm updated)
execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}"
    OUTPUT_VARIABLE new_revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(initial_revision STREQUAL new_revision)
    message(FATAL_ERROR "Fixture commit did not change the revision.")
endif()
check_metadata("${new_revision}" OFF)
