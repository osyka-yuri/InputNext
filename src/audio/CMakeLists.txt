add_library(inputnext_audio_native STATIC
    windows/native_port.cpp
    windows/wave_format.cpp
    windows/endpoint_snapshot.cpp
)
target_include_directories(inputnext_audio_native
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_audio_native PRIVATE ole32 uuid avrt)

inputnext_configure_target(inputnext_audio_native)

add_library(inputnext_audio STATIC
    audio_service.cpp
    service/control.cpp
    service/startup.cpp
    service/handoff.cpp
    service/retirement.cpp
    service/progress.cpp
    service/worker.cpp
    service/worker_setup.cpp
    service/worker_render.cpp
)
target_include_directories(inputnext_audio PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(inputnext_audio PUBLIC
    inputnext_output_execution inputnext_gameinput_output inputnext_audio_native
)
inputnext_configure_target(inputnext_audio)
