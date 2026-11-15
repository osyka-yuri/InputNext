add_library(inputnext_dinput_profile STATIC profile.cpp format.cpp)
target_include_directories(inputnext_dinput_profile
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
inputnext_configure_target(inputnext_dinput_profile)

add_library(inputnext_dinput STATIC
    catalogue/devices.cpp
    catalogue/discovery.cpp
    catalogue/identity.cpp
    com/device.cpp
    com/factory.cpp
    com/root.cpp
    input_access.cpp
    control_wait.cpp
    device_info.cpp
    device_properties.cpp
    property_abi.c
    system_input.cpp
)
target_include_directories(inputnext_dinput PUBLIC
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(inputnext_dinput PUBLIC
    inputnext_dinput_profile inputnext_core inputnext_gameinput_runtime inputnext_focus
    dxguid ole32 user32 bcrypt)
inputnext_configure_target(inputnext_dinput)
