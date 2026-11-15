if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(inputnext_dinput_definition dinput8_x86.def)
else()
    set(inputnext_dinput_definition dinput8.def)
endif()

add_library(inputnext_dinput8 SHARED
    runtime_resolver.cpp
    dinput_exports.cpp
    "${inputnext_dinput_definition}"
)
target_include_directories(inputnext_dinput8 PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
    "${PROJECT_SOURCE_DIR}/include"
)
# Keep app-local facades beside the runtime for sibling-path resolution.
set_target_properties(inputnext_dinput8 PROPERTIES
    OUTPUT_NAME dinput8
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/xinput"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/symbols"
)
inputnext_configure_target(inputnext_dinput8)
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    # The system DirectInput ABI fixes COM ordinals 2-5. Keep them despite LINK's
    # general advice against ordinal assignments for COM server exports.
    target_link_options(inputnext_dinput8 PRIVATE /IGNORE:4222)
endif()
add_dependencies(inputnext_dinput8 inputnext_runtime)

add_custom_target(inputnext_dinput_bundle DEPENDS
    inputnext_dinput8 inputnext_xinput_bundle)
