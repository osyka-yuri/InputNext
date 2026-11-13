if(NOT INPUTNEXT_BUILD_DIAGNOSTICS)
    return()
endif()

add_executable(inputnext_input_qualification
    main.cpp
    bootstrap.cpp
    capture_analysis.cpp
    capture_report.cpp
    gameinput_observation.cpp
    native_capture.cpp
    report.cpp
)
target_link_libraries(inputnext_input_qualification PRIVATE
    inputnext_gameinput_native inputnext_gameinput_sdk ole32 version
)
target_include_directories(inputnext_input_qualification PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(inputnext_input_qualification PRIVATE
    INPUTNEXT_GAMEINPUT_SDK_VERSION="${INPUTNEXT_GAMEINPUT_VERSION}"
)
set_target_properties(inputnext_input_qualification PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools/input_qualification"
)
inputnext_configure_target(inputnext_input_qualification)
inputnext_deploy_gameinput(inputnext_input_qualification)
