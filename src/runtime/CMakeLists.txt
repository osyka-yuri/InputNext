add_library(inputnext_runtime_session STATIC
    session/native_session.cpp
    session/native_session_prepare.cpp
    session/native_session_maintenance.cpp
    session/native_session_shutdown.cpp
    session/native_paths.cpp
)
target_include_directories(inputnext_runtime_session
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_runtime_session PRIVATE
    inputnext_gameinput_xinput
    inputnext_gameinput_output
    inputnext_output_authority
    inputnext_output_execution
    inputnext_focus
    inputnext_profiles
    inputnext_profile_application
    ole32
)

inputnext_configure_target(inputnext_runtime_session)

add_library(inputnext_runtime_query_front STATIC bootstrap/query_front.cpp)
target_include_directories(inputnext_runtime_query_front
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)

inputnext_configure_target(inputnext_runtime_query_front)

add_library(inputnext_runtime_core OBJECT
    abi/runtime_api.cpp
    bootstrap/bootstrap_owner.cpp
)
target_include_directories(inputnext_runtime_core
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_runtime_core PUBLIC
    inputnext_runtime_session inputnext_runtime_query_front
)

inputnext_configure_target(inputnext_runtime_core)

add_library(inputnext_runtime SHARED
    bootstrap/production_runtime.cpp
    inputnext_runtime.def
)
target_include_directories(inputnext_runtime PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(inputnext_runtime PRIVATE inputnext_runtime_core)

set_target_properties(inputnext_runtime PROPERTIES
    LINKER_LANGUAGE CXX
    OUTPUT_NAME inputnext_runtime
    PREFIX ""
    WINDOWS_EXPORT_ALL_SYMBOLS OFF
    RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/xinput"
    ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib"
    PDB_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/symbols"
)
inputnext_configure_target(inputnext_runtime)
inputnext_deploy_gameinput(inputnext_runtime)
