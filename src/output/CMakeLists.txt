add_library(inputnext_output_policy STATIC
    policy/body.cpp
    policy/policy.cpp
)
target_include_directories(inputnext_output_policy PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_output_policy)

add_library(inputnext_output_pcm STATIC
    pcm/body_pcm_policy.cpp
    pcm/body_pcm_store.cpp
    pcm/body_pcm_storage.cpp
    pcm/body_pcm_preparer.cpp
)
target_include_directories(inputnext_output_pcm
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
inputnext_configure_target(inputnext_output_pcm)

add_library(inputnext_body_playback STATIC
    playback/body_pcm_mixer.cpp
    playback/body_playback_bank.cpp
    playback/body_render_permit.cpp
)
target_include_directories(inputnext_body_playback
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_body_playback PUBLIC inputnext_output_pcm)
inputnext_configure_target(inputnext_body_playback)

add_library(inputnext_output_authority STATIC
    body/audio_route_control.cpp
    authority/incarnation.cpp
    authority/execution.cpp
    authority/state.cpp
    authority/cleanup.cpp
    authority/lifecycle.cpp
    authority/mutation.cpp
    authority/facts.cpp
    authority/snapshot.cpp
    body/pcm_collection.cpp
    body/playback_authority.cpp
    body/playback_lifecycle.cpp
    body/authority.cpp
    body/lifecycle.cpp
    composition/composition.cpp
    composition/internal_producer.cpp
    xinput/rumble.cpp
    projection/represented.cpp
)
target_include_directories(inputnext_output_authority
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_output_authority
    PUBLIC inputnext_output_policy inputnext_devices inputnext_focus inputnext_body_playback
    PRIVATE Ole32
)

inputnext_configure_target(inputnext_output_authority)

add_library(inputnext_output_execution_engine OBJECT
    execution/body_route.cpp
    execution/body_render_admission.cpp
    execution/drive.cpp
    execution/drive_policy.cpp
    execution/engine_state.cpp
    execution/progress.cpp
    execution/publication.cpp
    execution/refresh.cpp
    execution/registration.cpp
    execution/retirement.cpp
)
target_include_directories(inputnext_output_execution_engine
    PRIVATE
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_output_execution_engine PRIVATE inputnext_output_authority)

inputnext_configure_target(inputnext_output_execution_engine)

function(inputnext_add_output_executor target)
    add_library(${target} STATIC
        "${PROJECT_SOURCE_DIR}/src/output/render/control.cpp"
        "${PROJECT_SOURCE_DIR}/src/output/executor/executor.cpp"
        "${PROJECT_SOURCE_DIR}/src/output/executor/executor_coordinator.cpp"
        "${PROJECT_SOURCE_DIR}/src/output/executor/executor_registration.cpp"
        "${PROJECT_SOURCE_DIR}/src/output/executor/executor_workers.cpp"
        ${ARGN}
    )
    target_include_directories(${target}
        PUBLIC "${PROJECT_SOURCE_DIR}/include"
        PRIVATE "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(${target}
        PUBLIC inputnext_output_authority
        PRIVATE inputnext_output_execution_engine
    )

    inputnext_configure_target(${target})
endfunction()

inputnext_add_output_executor(inputnext_output_execution)
