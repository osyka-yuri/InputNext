add_library(inputnext_gameinput_probe_support STATIC supervisor.cpp)
target_include_directories(inputnext_gameinput_probe_support PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_gameinput_probe_support)

add_executable(inputnext_gameinput_probe
    main.cpp
    worker.cpp
    report.cpp
    history_observer.cpp
    history_report.cpp
)
set_target_properties(inputnext_gameinput_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools/gameinput_probe"
)
target_link_libraries(inputnext_gameinput_probe PRIVATE
    inputnext_gameinput_runtime inputnext_gameinput_probe_support ole32
)
inputnext_configure_target(inputnext_gameinput_probe)
inputnext_deploy_gameinput(inputnext_gameinput_probe)
