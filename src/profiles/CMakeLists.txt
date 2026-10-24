include("${PROJECT_SOURCE_DIR}/cmake/Toml.cmake")

add_library(inputnext_profiles STATIC
    detail/profile_stream.cpp
    detail/trigger_parser.cpp
    loader.cpp
    parser.cpp
    toml_implementation.cpp
)
target_include_directories(inputnext_profiles
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_profiles PRIVATE tomlplusplus::tomlplusplus)
target_compile_definitions(inputnext_profiles PRIVATE
    TOML_HEADER_ONLY=0
    TOML_ENABLE_FORMATTERS=0
    TOML_EXCEPTIONS=0
    TOML_MAX_NESTED_VALUES=32
)
set_source_files_properties(toml_implementation.cpp PROPERTIES COMPILE_DEFINITIONS TOML_IMPLEMENTATION=1)
inputnext_configure_target(inputnext_profiles)

add_library(inputnext_profile_application STATIC
    application.cpp
    application_retirement.cpp
)
target_include_directories(inputnext_profile_application
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_profile_application PRIVATE
    inputnext_output_authority inputnext_xinput
)
inputnext_configure_target(inputnext_profile_application)
