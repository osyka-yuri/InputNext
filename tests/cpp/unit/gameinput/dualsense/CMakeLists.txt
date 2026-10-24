add_executable(inputnext_dualsense_usb_unit_tests usb_report_tests.cpp)
target_include_directories(inputnext_dualsense_usb_unit_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(inputnext_dualsense_usb_unit_tests
    PRIVATE inputnext_dualsense_usb_codec Catch2::Catch2WithMain
)
inputnext_configure_target(inputnext_dualsense_usb_unit_tests)

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/results")
catch_discover_tests(inputnext_dualsense_usb_unit_tests
    ADD_TAGS_AS_LABELS
    PROPERTIES TIMEOUT 20
    OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/results"
    OUTPUT_SUFFIX .txt
)
