add_library(inputnext_output_policy STATIC
    policy/policy.cpp
)
target_include_directories(inputnext_output_policy PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_output_policy)

add_library(inputnext_output_authority STATIC
    authority/incarnation.cpp
    authority/execution.cpp
    authority/state.cpp
    authority/cleanup.cpp
    authority/lifecycle.cpp
    authority/mutation.cpp
    authority/facts.cpp
    authority/snapshot.cpp
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
    PUBLIC inputnext_output_policy inputnext_devices inputnext_focus
    PRIVATE Ole32
)

inputnext_configure_target(inputnext_output_authority)

add_library(inputnext_output_execution_engine OBJECT
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
