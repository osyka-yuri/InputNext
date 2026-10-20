add_library(inputnext_xinput STATIC
    service.cpp
    queries.cpp
    query_projection.cpp
    translation.cpp
)
target_include_directories(inputnext_xinput
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_xinput PUBLIC inputnext_devices inputnext_focus)
target_compile_definitions(inputnext_xinput INTERFACE WIN32_LEAN_AND_MEAN NOMINMAX)
inputnext_configure_target(inputnext_xinput)
