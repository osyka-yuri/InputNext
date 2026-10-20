add_library(inputnext_focus STATIC
    current_process_source.cpp
    observer.cpp
)
target_include_directories(inputnext_focus PUBLIC "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(inputnext_focus PUBLIC User32)

inputnext_configure_target(inputnext_focus)
