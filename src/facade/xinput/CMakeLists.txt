function(inputnext_add_xinput_facade target output_name definition flavor)
    add_library(${target} SHARED
        runtime_resolver.cpp
        xinput_exports.cpp
        "${definition}"
    )
    target_include_directories(${target} PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/include"
    )
    target_compile_definitions(${target} PRIVATE
        INPUTNEXT_XINPUT_FLAVOR=${flavor}
    )
    set_target_properties(${target} PROPERTIES
        OUTPUT_NAME "${output_name}"
        PREFIX ""
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
        RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/xinput"
        ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
        PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/symbols"
    )
    inputnext_configure_target(${target})
    add_dependencies(${target} inputnext_runtime)
endfunction()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(inputnext_xinput13_definition xinput13_x86.def)
    set(inputnext_xinput14_definition xinput14_x86.def)
    set(inputnext_xinput910_definition xinput910_x86.def)
else()
    set(inputnext_xinput13_definition xinput13.def)
    set(inputnext_xinput14_definition xinput14.def)
    set(inputnext_xinput910_definition xinput910.def)
endif()

inputnext_add_xinput_facade(inputnext_xinput13 xinput1_3 ${inputnext_xinput13_definition} 0)
inputnext_add_xinput_facade(inputnext_xinput14 xinput1_4 ${inputnext_xinput14_definition} 1)
inputnext_add_xinput_facade(
    inputnext_xinput910 xinput9_1_0 ${inputnext_xinput910_definition} 2)

# Retain the 1.4 import so the loader resolves it before this facade initializes.
# Link an import library generated from the one required public import instead of
# the full facade import library. Instrumented facade builds export runtime
# symbols, which must not become part of the dependent facades' import tables.
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(inputnext_xinput14_import_definition xinput14_import_x86.def)
    set(inputnext_xinput14_force_import /INCLUDE:__imp__XInputGetState@8)
else()
    set(inputnext_xinput14_import_definition xinput14_import.def)
    set(inputnext_xinput14_force_import /INCLUDE:__imp_XInputGetState)
endif()
set(inputnext_xinput14_import_definition_path
    "${CMAKE_CURRENT_SOURCE_DIR}/${inputnext_xinput14_import_definition}")
set(inputnext_xinput14_import_library
    "${PROJECT_BINARY_DIR}/lib/xinput14-import.lib")
add_custom_command(OUTPUT "${inputnext_xinput14_import_library}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${PROJECT_BINARY_DIR}/lib"
    COMMAND "${CMAKE_AR}" /nologo "/machine:${inputnext_compiler_architecture}"
        "/def:${inputnext_xinput14_import_definition_path}"
        "/out:${inputnext_xinput14_import_library}"
    DEPENDS "${inputnext_xinput14_import_definition_path}"
    VERBATIM
)
add_custom_target(inputnext_xinput14_import_library DEPENDS
    "${inputnext_xinput14_import_library}")
# Microsoft lib.exe also writes an unused .exp; llvm-lib.exe does not.
set_property(TARGET inputnext_xinput14_import_library APPEND PROPERTY ADDITIONAL_CLEAN_FILES
    "${PROJECT_BINARY_DIR}/lib/xinput14-import.exp")

foreach(target IN ITEMS inputnext_xinput13 inputnext_xinput910)
    target_link_libraries(${target} PRIVATE "${inputnext_xinput14_import_library}")
    target_link_options(${target} PRIVATE "${inputnext_xinput14_force_import}")
    add_dependencies(${target} inputnext_xinput14 inputnext_xinput14_import_library)
endforeach()

add_custom_target(inputnext_xinput_bundle DEPENDS
    inputnext_runtime inputnext_xinput13 inputnext_xinput14 inputnext_xinput910
)
