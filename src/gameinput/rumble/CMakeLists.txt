add_library(inputnext_gameinput_rumble STATIC rumble_sink.cpp)
target_include_directories(inputnext_gameinput_rumble
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_gameinput_rumble PUBLIC inputnext_gameinput_runtime)
inputnext_configure_target(inputnext_gameinput_rumble)
