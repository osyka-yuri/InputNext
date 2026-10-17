add_library(inputnext_devices STATIC
    device_registry.cpp
    input_binding.cpp
)
target_link_libraries(inputnext_devices PUBLIC inputnext_input)
inputnext_configure_target(inputnext_devices)
