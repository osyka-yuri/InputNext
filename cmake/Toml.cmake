include(FetchContent)

FetchContent_Declare(tomlplusplus
    URL https://codeload.github.com/marzer/tomlplusplus/tar.gz/refs/tags/v${INPUTNEXT_TOML_VERSION}
    URL_HASH SHA256=8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155
    PATCH_COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=<SOURCE_DIR>"
        -P "${PROJECT_SOURCE_DIR}/cmake/LimitTomlTables.cmake"
    SYSTEM
    EXCLUDE_FROM_ALL
)
FetchContent_MakeAvailable(tomlplusplus)
