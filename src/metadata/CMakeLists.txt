add_library(inputnext_metadata_policy STATIC policy.cpp)
target_include_directories(inputnext_metadata_policy PUBLIC "${PROJECT_SOURCE_DIR}/include")
inputnext_configure_target(inputnext_metadata_policy)

if(INPUTNEXT_BUILD_GAMEINPUT)
    add_library(inputnext_windows_metadata STATIC
        windows/provider.cpp
        windows/os_port.cpp
    )
    target_include_directories(inputnext_windows_metadata PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
    )
    target_link_libraries(inputnext_windows_metadata
        PUBLIC inputnext_metadata_policy
        PRIVATE setupapi cfgmgr32 ole32 oleaut32 propsys uuid
    )

    inputnext_configure_target(inputnext_windows_metadata)
endif()
