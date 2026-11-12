add_library(inputnext_input STATIC
    latest_value.cpp
    program.cpp
    touch_resolver.cpp
)
target_include_directories(inputnext_input PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_input)
