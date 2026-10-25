add_library(inputnext_core STATIC
    core.cpp
    device_catalogue.cpp
    device_identity.cpp
    history_status.cpp
    latest.cpp
    reader_delivery.cpp
    readers.cpp
    selection.cpp
    target_status.cpp
)
target_include_directories(inputnext_core
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_core
    PUBLIC inputnext_devices inputnext_gameinput_runtime inputnext_focus inputnext_xinput
    PRIVATE bcrypt
)
inputnext_configure_target(inputnext_core)
