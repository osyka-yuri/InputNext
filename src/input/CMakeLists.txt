add_library(inputnext_input STATIC latest_value.cpp)
target_include_directories(inputnext_input PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_input)
