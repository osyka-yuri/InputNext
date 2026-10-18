include(FetchContent)

set(INPUTNEXT_GAMEINPUT_VERSION "4.0.300")
FetchContent_Declare(microsoft_gameinput
    URL https://api.nuget.org/v3-flatcontainer/microsoft.gameinput/${INPUTNEXT_GAMEINPUT_VERSION}/microsoft.gameinput.${INPUTNEXT_GAMEINPUT_VERSION}.nupkg
    URL_HASH SHA256=ae7dc3a311466a72ddcfd6c5ccd5b1e917be890658236fa9791e937115ac2d79
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(microsoft_gameinput)

add_library(inputnext_gameinput_sdk INTERFACE)
target_include_directories(inputnext_gameinput_sdk SYSTEM INTERFACE "${microsoft_gameinput_SOURCE_DIR}/native/include")

function(inputnext_deploy_gameinput target)
    FetchContent_GetProperties(microsoft_gameinput SOURCE_DIR gameinput_source)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${gameinput_source}/native/bin/${inputnext_gameinput_architecture}/GameInputRedist.dll"
            "$<TARGET_FILE_DIR:${target}>/GameInputRedist.dll"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${gameinput_source}/native/bin/${inputnext_gameinput_architecture}/GameInputRawInputProxy.exe"
            "$<TARGET_FILE_DIR:${target}>/GameInputRawInputProxy.exe"
        VERBATIM
    )
endfunction()
