if(NOT TARGET inputnext_xinput_bundle)
    message(FATAL_ERROR "Distribution requires INPUTNEXT_BUILD_GAMEINPUT=ON.")
endif()

set(inputnext_distribution_name "InputNext-${PROJECT_VERSION}-windows-${inputnext_compiler_architecture}")
set(inputnext_distribution_targets
    inputnext_runtime inputnext_xinput13 inputnext_xinput14 inputnext_xinput910)
install(TARGETS ${inputnext_distribution_targets}
    RUNTIME DESTINATION bin COMPONENT Runtime
)
FetchContent_GetProperties(microsoft_gameinput SOURCE_DIR gameinput_source)
install(FILES
    "${gameinput_source}/native/bin/${inputnext_gameinput_architecture}/GameInputRedist.dll"
    "${gameinput_source}/native/bin/${inputnext_gameinput_architecture}/GameInputRawInputProxy.exe"
    DESTINATION bin COMPONENT Runtime
)
install(FILES LICENSE README.md THIRD_PARTY_NOTICES.md DESTINATION . COMPONENT Runtime)
install(DIRECTORY licenses/ DESTINATION licenses COMPONENT Runtime)
install(DIRECTORY docs/ DESTINATION docs COMPONENT Runtime FILES_MATCHING PATTERN "*.md")
install(FILES sdk/input_next_core.h sdk/input_next_effects.h DESTINATION sdk COMPONENT Runtime)
install(FILES examples/inputnext.toml examples/CMakeLists.txt DESTINATION examples COMPONENT Runtime)
foreach(example IN ITEMS core effects)
    install(DIRECTORY "examples/${example}/" DESTINATION "examples/${example}"
        COMPONENT Runtime FILES_MATCHING PATTERN "*.c" PATTERN "*.cpp" PATTERN "*.h")
endforeach()
foreach(target IN LISTS inputnext_distribution_targets)
    install(FILES "$<TARGET_PDB_FILE:${target}>" DESTINATION symbols COMPONENT Symbols)
endforeach()

configure_file("${PROJECT_SOURCE_DIR}/packaging/build-info.json.in"
    "${PROJECT_BINARY_DIR}/distribution/build-info-base.json" @ONLY)
configure_file("${PROJECT_SOURCE_DIR}/cmake/WriteBuildInfo.cmake.in"
    "${PROJECT_BINARY_DIR}/distribution/WriteBuildInfo.cmake" @ONLY)
install(SCRIPT "${PROJECT_BINARY_DIR}/distribution/WriteBuildInfo.cmake" COMPONENT Runtime)
install(SCRIPT "${PROJECT_BINARY_DIR}/distribution/WriteBuildInfo.cmake" COMPONENT Symbols)
install(SCRIPT "${PROJECT_SOURCE_DIR}/cmake/WritePackageManifest.cmake" COMPONENT Runtime)
install(SCRIPT "${PROJECT_SOURCE_DIR}/cmake/WritePackageManifest.cmake" COMPONENT Symbols)

set(CPACK_GENERATOR ZIP)
set(CPACK_PACKAGE_NAME InputNext)
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DIRECTORY "${PROJECT_BINARY_DIR}/packages")
set(CPACK_PACKAGE_FILE_NAME "${inputnext_distribution_name}")
set(CPACK_ARCHIVE_COMPONENT_INSTALL ON)
set(CPACK_COMPONENTS_ALL Runtime Symbols)
set(CPACK_ARCHIVE_RUNTIME_FILE_NAME "${inputnext_distribution_name}")
set(CPACK_ARCHIVE_SYMBOLS_FILE_NAME "${inputnext_distribution_name}-symbols")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_COMPONENT_INCLUDE_TOPLEVEL_DIRECTORY OFF)
set(CPACK_VERBATIM_VARIABLES YES)
include(CPack)
add_custom_target(inputnext_distribution
    COMMAND "${CMAKE_CPACK_COMMAND}" --config "${PROJECT_BINARY_DIR}/CPackConfig.cmake"
    COMMAND "${CMAKE_COMMAND}"
        "-DPACKAGE_DIRECTORY=${CPACK_PACKAGE_DIRECTORY}"
        "-DPACKAGE_NAME=${inputnext_distribution_name}"
        -P "${PROJECT_SOURCE_DIR}/cmake/HashPackages.cmake"
    DEPENDS inputnext_xinput_bundle
    VERBATIM
)
