add_library(inputnext_dualsense_usb_codec STATIC
    feedback_payload.cpp
    usb_report.cpp
)
target_link_libraries(inputnext_dualsense_usb_codec PUBLIC inputnext_output_policy)
target_include_directories(inputnext_dualsense_usb_codec PRIVATE "${PROJECT_SOURCE_DIR}/src")
inputnext_configure_target(inputnext_dualsense_usb_codec)

if(INPUTNEXT_BUILD_GAMEINPUT)
    add_library(inputnext_gameinput_dualsense_usb STATIC usb_sink.cpp)
    target_link_libraries(inputnext_gameinput_dualsense_usb
        PUBLIC inputnext_gameinput_runtime inputnext_dualsense_usb_codec
    )
    target_include_directories(inputnext_gameinput_dualsense_usb PRIVATE "${PROJECT_SOURCE_DIR}/src")
    inputnext_configure_target(inputnext_gameinput_dualsense_usb)
endif()
