option(INPUTNEXT_PRODUCTION "Build optimized distribution binaries" OFF)
set(INPUTNEXT_PGO_MODE "Off" CACHE STRING "Production PGO mode")
set_property(CACHE INPUTNEXT_PGO_MODE PROPERTY STRINGS Off Instrument Optimize)
set(INPUTNEXT_PGO_DIRECTORY "${PROJECT_BINARY_DIR}/pgo" CACHE PATH "MSVC runtime profile data")

function(inputnext_prepare_production_configuration)
    if(NOT INPUTNEXT_PGO_MODE MATCHES "^(Off|Instrument|Optimize)$")
        message(FATAL_ERROR "INPUTNEXT_PGO_MODE must be Off, Instrument, or Optimize.")
    endif()
    if(NOT INPUTNEXT_PRODUCTION)
        if(NOT INPUTNEXT_PGO_MODE STREQUAL "Off")
            message(FATAL_ERROR "PGO requires INPUTNEXT_PRODUCTION=ON.")
        endif()
        return()
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR "The production profile requires the MSVC C and C++ compilers.")
    endif()
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES)
        message(FATAL_ERROR "The production profile requires a single-configuration Release build.")
    endif()
    if(NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Default")
        message(FATAL_ERROR "Production and native diagnostic profiles must use separate builds.")
    endif()
    # Remove the inherited inline level to avoid conflicting with the production /Ob3.
    foreach(language IN ITEMS C CXX)
        string(REGEX REPLACE "(^| )[/-]Ob[0-3]( |$)" " " flags "${CMAKE_${language}_FLAGS_RELEASE}")
        set(CMAKE_${language}_FLAGS_RELEASE "${flags}" PARENT_SCOPE)
    endforeach()
    if(NOT INPUTNEXT_PGO_MODE STREQUAL "Off")
        if(NOT IS_ABSOLUTE "${INPUTNEXT_PGO_DIRECTORY}" OR INPUTNEXT_PGO_DIRECTORY MATCHES "[,;]")
            message(FATAL_ERROR "INPUTNEXT_PGO_DIRECTORY must be an absolute path without commas or semicolons.")
        endif()
        file(MAKE_DIRECTORY "${INPUTNEXT_PGO_DIRECTORY}")
        if(INPUTNEXT_PGO_MODE STREQUAL "Instrument")
            string(RANDOM LENGTH 32 ALPHABET 0123456789abcdef campaign)
            file(REMOVE "${INPUTNEXT_PGO_DIRECTORY}/trained/ready"
                "${INPUTNEXT_PGO_DIRECTORY}/instrumented")
            file(WRITE "${INPUTNEXT_PGO_DIRECTORY}/campaign" "${campaign}")
        elseif(INPUTNEXT_PGO_MODE STREQUAL "Optimize")
            execute_process(COMMAND "${CMAKE_COMMAND}"
                "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
                -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VerifyPgoProfile.cmake"
                COMMAND_ERROR_IS_FATAL ANY)
        endif()
    endif()
endfunction()

function(inputnext_configure_production target)
    if(NOT INPUTNEXT_PRODUCTION)
        return()
    endif()
    set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    target_compile_options(${target} PRIVATE /O2 /Ob3 /Gw /Gy /Zi /fp:precise)
    get_target_property(type ${target} TYPE)
    if(type STREQUAL "EXECUTABLE" OR type STREQUAL "SHARED_LIBRARY")
        target_link_options(${target} PRIVATE
            /WX /DEBUG:FULL /OPT:REF,ICF /INCREMENTAL:NO /DYNAMICBASE /NXCOMPAT
            "/PDBALTPATH:%_PDB%"
        )
        if(inputnext_compiler_architecture STREQUAL "x64")
            target_link_options(${target} PRIVATE /HIGHENTROPYVA)
        endif()
    endif()
    if(target STREQUAL "inputnext_runtime")
        get_target_property(name ${target} OUTPUT_NAME)
        set(profile "${INPUTNEXT_PGO_DIRECTORY}/${name}.pgd")
        if(INPUTNEXT_PGO_MODE STREQUAL "Instrument")
            target_link_options(${target} PRIVATE "/GENPROFILE:EXACT,PGD=${profile}")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
                "${INPUTNEXT_PGO_DIRECTORY}/campaign")
            add_custom_command(TARGET ${target} PRE_LINK
                COMMAND "${CMAKE_COMMAND}"
                    "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
                    "-DCOUNTER_DIRECTORY=$<TARGET_FILE_DIR:${target}>"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/BeginPgoInstrumentation.cmake"
                VERBATIM)
            get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
            get_filename_component(toolset_directory "${compiler_directory}/../../.." ABSOLUTE)
            find_file(pgo_runtime NAMES pgort140.dll REQUIRED NO_CACHE
                PATHS "${compiler_directory}"
                    "${toolset_directory}/bin/Hostx86/${inputnext_compiler_architecture}"
                NO_DEFAULT_PATH)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                    "${pgo_runtime}" "$<TARGET_FILE_DIR:${target}>/pgort140.dll"
                COMMAND "${CMAKE_COMMAND}" -E copy
                    "${INPUTNEXT_PGO_DIRECTORY}/campaign"
                    "${INPUTNEXT_PGO_DIRECTORY}/instrumented"
                VERBATIM)
        elseif(INPUTNEXT_PGO_MODE STREQUAL "Optimize")
            set(profile "${INPUTNEXT_PGO_DIRECTORY}/trained/${name}.pgd")
            if(NOT EXISTS "${profile}")
                message(FATAL_ERROR "Missing trained profile for ${target}: ${profile}")
            endif()
            set(link_profile "${INPUTNEXT_PGO_DIRECTORY}/link/${name}.pgd")
            target_link_options(${target} PRIVATE "/USEPROFILE:PGD=${link_profile}")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
                "${profile}" "${INPUTNEXT_PGO_DIRECTORY}/trained/ready"
                "${INPUTNEXT_PGO_DIRECTORY}/campaign" "${INPUTNEXT_PGO_DIRECTORY}/instrumented"
                "${INPUTNEXT_PGO_DIRECTORY}/${name}.pgd")
            add_custom_command(TARGET ${target} PRE_LINK
                COMMAND "${CMAKE_COMMAND}"
                    "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
                    -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/VerifyPgoProfile.cmake"
                COMMAND "${CMAKE_COMMAND}" -E make_directory "${INPUTNEXT_PGO_DIRECTORY}/link"
                COMMAND "${CMAKE_COMMAND}" -E copy "${profile}" "${link_profile}"
                VERBATIM)
        endif()
        if(NOT INPUTNEXT_PGO_MODE STREQUAL "Instrument")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E rm -f "$<TARGET_FILE_DIR:${target}>/pgort140.dll"
                VERBATIM)
        endif()
    endif()
endfunction()

function(inputnext_add_pgo_training_target)
    if(NOT INPUTNEXT_PRODUCTION OR NOT INPUTNEXT_PGO_MODE STREQUAL "Instrument")
        return()
    endif()
    get_filename_component(compiler_directory "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(pgo_manager NAMES pgomgr REQUIRED NO_CACHE
        PATHS "${compiler_directory}" NO_DEFAULT_PATH)
    add_custom_target(inputnext_pgo_merge
        COMMAND "${CMAKE_COMMAND}"
            "-DPROFILE_DIRECTORY=${INPUTNEXT_PGO_DIRECTORY}"
            "-DCOUNTER_DIRECTORY=$<TARGET_FILE_DIR:inputnext_runtime>"
            "-DPGO_MANAGER=${pgo_manager}"
            -P "${PROJECT_SOURCE_DIR}/cmake/MergePgoProfiles.cmake"
        DEPENDS inputnext_xinput_bundle
        VERBATIM)
endfunction()
