add_library(inputnext_dinput_profile STATIC profile.cpp format.cpp)
target_include_directories(inputnext_dinput_profile
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
inputnext_configure_target(inputnext_dinput_profile)

add_library(inputnext_dinput STATIC
    buffered/event_registration.cpp
    buffered/service.cpp
    buffered/subscription.cpp
    catalogue/devices.cpp
    catalogue/discovery.cpp
    catalogue/identity.cpp
    com/device.cpp
    com/enumeration.cpp
    com/factory.cpp
    com/root.cpp
    exclusive/actor.cpp
    exclusive/lease.cpp
    exclusive/ownership.cpp
    force_feedback/contribution.cpp
    force_feedback/effect.cpp
    force_feedback/engine.cpp
    force_feedback/parameters.cpp
    force_feedback/playback.cpp
    force_feedback/readback.cpp
    force_feedback/service.cpp
    input_access.cpp
    control_wait.cpp
    device_input.cpp
    device_acquisition.cpp
    device_info.cpp
    device_properties.cpp
    property_abi.c
    system_input.cpp
)
target_include_directories(inputnext_dinput PUBLIC
    "${PROJECT_SOURCE_DIR}/include" "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(inputnext_dinput PUBLIC
    inputnext_dinput_profile inputnext_core inputnext_gameinput_runtime inputnext_focus
    inputnext_output_authority inputnext_output_execution
    dxguid ole32 user32 bcrypt)
inputnext_configure_target(inputnext_dinput)
