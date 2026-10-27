add_library(inputnext_effects STATIC
    backend_observation.cpp
    error_mapping.cpp
    identity.cpp
    progress_evaluation.cpp
    service.cpp
    service_body.cpp
    service_cleanup.cpp
    service_contexts.cpp
    service_discrete.cpp
    service_progress.cpp
    service_resolution.cpp
)
target_include_directories(inputnext_effects
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_effects PUBLIC
    inputnext_core
    inputnext_gameinput_output
    inputnext_output_authority
    inputnext_audio
)
inputnext_configure_target(inputnext_effects)
