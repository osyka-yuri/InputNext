option(INPUTNEXT_ENABLE_CLANG_TIDY "Run clang-tidy while compiling project targets" OFF)
if(INPUTNEXT_ENABLE_CLANG_TIDY)
    find_program(INPUTNEXT_CLANG_TIDY NAMES clang-tidy REQUIRED)
    execute_process(COMMAND "${INPUTNEXT_CLANG_TIDY}" --version
        OUTPUT_VARIABLE inputnext_clang_tidy_version
        COMMAND_ERROR_IS_FATAL ANY
    )
    if(NOT inputnext_clang_tidy_version MATCHES "LLVM version 23\\.1\\.3([^0-9.]|$)")
        message(FATAL_ERROR "InputNext requires clang-tidy 23.1.3.")
    endif()
endif()

function(inputnext_configure_native_target target)
    target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(${target} PRIVATE
        /W4 /WX
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
        $<$<COMPILE_LANGUAGE:CXX>:/permissive->
        /utf-8
        $<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>
    )
    if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        target_compile_options(${target} PRIVATE
            /Zc:preprocessor
            /sdl
            /options:strict
            /w44242 /w44254 /w44287 /w44296 /w44062 /w44265 /w44365 /w44388
        )
    endif()
    inputnext_configure_native_analysis(${target})
    inputnext_configure_sanitizer(${target})
    inputnext_configure_production(${target})
    if(INPUTNEXT_ENABLE_CLANG_TIDY)
        if(inputnext_compiler_architecture STREQUAL "x86")
            set(analysis_target i686-pc-windows-msvc)
        else()
            set(analysis_target x86_64-pc-windows-msvc)
        endif()
        set_property(TARGET ${target} PROPERTY CXX_CLANG_TIDY
            "${INPUTNEXT_CLANG_TIDY};--extra-arg=/EHsc;--extra-arg=--target=${analysis_target}")
        set_property(TARGET ${target} PROPERTY C_CLANG_TIDY
            "${INPUTNEXT_CLANG_TIDY};--extra-arg=--target=${analysis_target}")
        get_target_property(sources ${target} SOURCES)
        foreach(source IN LISTS sources)
            if(source MATCHES "\\.(c|cpp|cxx|cc)$")
                set_property(SOURCE "${source}" APPEND PROPERTY OBJECT_DEPENDS "${PROJECT_SOURCE_DIR}/.clang-tidy")
            endif()
        endforeach()
    endif()
endfunction()

function(inputnext_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_23)
    inputnext_configure_native_target(${target})
endfunction()

function(inputnext_configure_c_target target)
    target_compile_features(${target} PRIVATE c_std_17)
    set_target_properties(${target} PROPERTIES C_STANDARD_REQUIRED ON)
    inputnext_configure_native_target(${target})
endfunction()
