include(FetchContent)

FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v${INPUTNEXT_TOML_VERSION}
    URL_HASH SHA256=4821c769370a6489b6a37814f16f3769eee0f2e733a5c129d5d99e2515a95c2b
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${PROJECT_SOURCE_DIR}/cmake/LimitTomlTables.cmake"
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(tomlplusplus)
