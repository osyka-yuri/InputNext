# Configure opt-in diagnostics that require the native Microsoft compiler.

function(inputnext_validate_native_analysis_profile)
    if(NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Analyze")
        return()
    endif()

    if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
        message(FATAL_ERROR
            "The Analyze profile requires the native Microsoft C++ compiler. "
            "Detected ${CMAKE_CXX_COMPILER_ID}/${CMAKE_CXX_COMPILER_FRONTEND_VARIANT}."
        )
    endif()
endfunction()

function(inputnext_configure_native_analysis target)
    if(NOT INPUTNEXT_NATIVE_PROFILE STREQUAL "Analyze")
        return()
    endif()

    target_compile_options(${target} PRIVATE
        /analyze
        /analyze:external-
        /external:env:INCLUDE
        /external:W0
        /analyze:autolog:ext
        .sarif
    )
endfunction()
