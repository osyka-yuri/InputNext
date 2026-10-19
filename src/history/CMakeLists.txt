add_library(inputnext_history STATIC
    open.cpp
    state.cpp
    read.cpp
    ring.cpp
    store.cpp
)
target_include_directories(inputnext_history
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(inputnext_history PUBLIC inputnext_input)
inputnext_configure_target(inputnext_history)
