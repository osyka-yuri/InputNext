#include "dll_test_support.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <string_view>
#include <system_error>

#ifndef INPUTNEXT_XINPUT_BUNDLE_DIRECTORY
#    error INPUTNEXT_XINPUT_BUNDLE_DIRECTORY must identify the actual production output directory.
#endif
#ifndef INPUTNEXT_XINPUT_TABLE_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_TABLE_FIXTURE_PATH must identify the table fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_EXTERNAL_CALLBACK_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_EXTERNAL_CALLBACK_FIXTURE_PATH must identify its external callback fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_INVALID_SIZE_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_INVALID_SIZE_FIXTURE_PATH must identify the invalid-size fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_INVALID_VERSION_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_INVALID_VERSION_FIXTURE_PATH must identify the invalid-version fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_MISSING_CALLBACK_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_MISSING_CALLBACK_FIXTURE_PATH must identify the missing-callback fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_NULL_TABLE_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_NULL_TABLE_FIXTURE_PATH must identify the null-table fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_MISSING_QUERY_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_MISSING_QUERY_FIXTURE_PATH must identify the missing-query fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_AR1_FIXTURE_PATH
#    error INPUTNEXT_XINPUT_AR1_FIXTURE_PATH must identify the AR1 rejecting-pin fixture DLL.
#endif
#ifndef INPUTNEXT_XINPUT_IMPORT_PROBE_PATH
#    error INPUTNEXT_XINPUT_IMPORT_PROBE_PATH must identify the normally imported probe executable.
#endif
#ifndef INPUTNEXT_XINPUT13_IMPORT_PROBE_PATH
#    error INPUTNEXT_XINPUT13_IMPORT_PROBE_PATH must identify the XInput 1.3 import probe executable.
#endif

namespace {

using namespace inputnext::xinput_dll_tests;

[[nodiscard]] std::filesystem::path bundle_directory() {
    return std::filesystem::path{INPUTNEXT_XINPUT_BUNDLE_DIRECTORY};
}

[[nodiscard]] std::filesystem::path table_fixture_path() {
    return std::filesystem::path{INPUTNEXT_XINPUT_TABLE_FIXTURE_PATH};
}

[[nodiscard]] std::filesystem::path external_callback_fixture_path() {
    return std::filesystem::path{INPUTNEXT_XINPUT_EXTERNAL_CALLBACK_FIXTURE_PATH};
}

[[nodiscard]] std::array<std::filesystem::path, 4U> malformed_fixture_paths() {
    return {std::filesystem::path{INPUTNEXT_XINPUT_INVALID_SIZE_FIXTURE_PATH},
        std::filesystem::path{INPUTNEXT_XINPUT_INVALID_VERSION_FIXTURE_PATH},
        std::filesystem::path{INPUTNEXT_XINPUT_MISSING_CALLBACK_FIXTURE_PATH},
        std::filesystem::path{INPUTNEXT_XINPUT_NULL_TABLE_FIXTURE_PATH}};
}

[[nodiscard]] std::filesystem::path missing_query_fixture_path() {
    return std::filesystem::path{INPUTNEXT_XINPUT_MISSING_QUERY_FIXTURE_PATH};
}

[[nodiscard]] std::filesystem::path ar1_fixture_path() {
    return std::filesystem::path{INPUTNEXT_XINPUT_AR1_FIXTURE_PATH};
}

[[nodiscard]] std::filesystem::path import_probe_path() {
    return std::filesystem::path{INPUTNEXT_XINPUT_IMPORT_PROBE_PATH};
}

void require_child_success(const child_result& result, const std::string_view output_marker) {
    INFO(result.output);
    INFO(result.errors);
    REQUIRE(!result.process.timed_out);
    REQUIRE(result.process.cleanup_confirmed);
    REQUIRE(result.process.exit_code == 0U);
    CHECK(result.output.contains(output_marker));
}

[[nodiscard]] child_result run_isolated(const wchar_t* const scenario,
    const std::filesystem::path& directory,
    const std::chrono::milliseconds timeout = std::chrono::seconds{30}) {
    return run_probe({scenario, directory.native()}, timeout, directory);
}

void copy_facade(const std::filesystem::path& bundle,
    const std::filesystem::path& destination,
    const wchar_t* const name) {
    copy_fixture(bundle / name, destination / name);
}

void copy_external_callback_fixture(const std::filesystem::path& destination) {
    copy_fixture(external_callback_fixture_path(),
        destination / L"inputnext_external_callback.dll");
}

void copy_production_bundle(const std::filesystem::path& bundle,
    const std::filesystem::path& destination) {
    constexpr std::array<const wchar_t*, 6U> names{L"xinput1_3.dll",
        L"xinput1_4.dll",
        L"xinput9_1_0.dll",
        L"inputnext_runtime.dll",
        L"GameInputRedist.dll",
        L"GameInputRawInputProxy.exe"};
    for (const auto* const name : names) {
        copy_fixture(bundle / name, destination / name);
    }
}

void check_temporary_directory_cleanup(temporary_directory& directory) {
    const std::error_code error = directory.cleanup();
    if (error) {
        INFO("temporary test directory cleanup failed: " + error.message());
    }
    CHECK(error.value() == 0);
}

} // namespace

TEST_CASE("xinput.dll.production_bundle_lazy_shared_runtime_reload",
    "[integration][xinput][dll][process]") {
    const auto result =
        inputnext::xinput_dll_tests::run_probe({L"production", bundle_directory().native()},
            std::chrono::seconds{45});
    require_child_success(result, "production_table_state=");
}

TEST_CASE("core.dll.query_preserves_the_shared_provider_without_native_startup",
    "[integration][core][dll][process][abi]") {
    const auto result =
        inputnext::xinput_dll_tests::run_probe({L"core-query", bundle_directory().native()},
            std::chrono::seconds{30});
    require_child_success(result, "core_query_lifetime=passed");
}

TEST_CASE("xinput.dll.companion_loads_before_runtime_and_unloads_with_facade",
    "[integration][xinput][dll][process][lifetime]") {
    const auto result = run_isolated(L"companion", bundle_directory());
    require_child_success(result, "xinput_companion_load=passed");
}

TEST_CASE("xinput.dll.companion_preserves_a_preloaded_system_module",
    "[integration][xinput][dll][process][lifetime]") {
    const auto result = run_isolated(L"companion-preloaded", bundle_directory());
    require_child_success(result, "xinput_companion_preloaded=passed");
}

TEST_CASE("xinput.dll.missing_companion_can_resolve_to_system_xinput",
    "[integration][xinput][dll][process][lifetime]") {
    temporary_directory directory{L"missing-companion"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_3.dll");
    copy_facade(bundle_directory(), directory.path(), L"xinput9_1_0.dll");
    const auto result = run_isolated(L"companion-missing", directory.path());
    require_child_success(result, "xinput_companion_missing=passed");
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.absolute_system_path_can_load_a_separate_companion",
    "[integration][xinput][dll][process][lifetime]") {
    const auto result = run_isolated(L"companion-system-path", bundle_directory());
    require_child_success(result, "xinput_companion_system_path=passed");
}

TEST_CASE("xinput.dll.normal_import_executable_uses_app_local_bundle",
    "[integration][xinput][dll][process][import]") {
    temporary_directory directory{L"normal-import"};
    copy_production_bundle(bundle_directory(), directory.path());
    const bool importing13 = GENERATE(false, true);
    const auto source = importing13 ? std::filesystem::path{INPUTNEXT_XINPUT13_IMPORT_PROBE_PATH}
                                    : import_probe_path();
    INFO("Importing XInput " << (importing13 ? "1.3" : "1.4"));
    const auto staged_executable = directory.path() / source.filename();
    copy_fixture(source, staged_executable);

    const auto result =
        run_import_probe(staged_executable, std::chrono::seconds{30}, directory.path());
    require_child_success(result, "xinput_import_route=app-local");
    if (importing13) {
        CHECK(result.output.contains("xinput_import13_startup=app-local-lazy"));
    }
    CHECK((result.output.contains("xinput_import_call=success") ||
           result.output.contains("xinput_import_call=disconnected")));
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.table_fixture_dispatches_all_abi_callbacks",
    "[integration][xinput][dll][process][abi]") {
    inputnext::xinput_dll_tests::temporary_directory directory{L"table-dispatch"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_3.dll");
    copy_facade(bundle_directory(), directory.path(), L"xinput1_4.dll");
    inputnext::xinput_dll_tests::copy_fixture(table_fixture_path(),
        directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(directory.path());

    const auto result = run_isolated(L"table-dispatch", directory.path());
    require_child_success(result, "fixture_callbacks=13");
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.valid_provider_is_retained_without_preload_and_after_reload",
    "[integration][xinput][dll][process][lifetime]") {
    temporary_directory directory{L"provider-lifetime"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_4.dll");
    copy_fixture(table_fixture_path(), directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(directory.path());

    const auto result = run_isolated(L"provider-lifetime", directory.path());
    require_child_success(result, "provider_lifetime=passed");
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.external_provider_tables_and_callbacks_are_rejected",
    "[integration][xinput][dll][process][lifetime]") {
    temporary_directory directory{L"external-addresses"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_4.dll");
    copy_fixture(table_fixture_path(), directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(directory.path());

    const auto result = run_isolated(L"external-addresses", directory.path());
    require_child_success(result, "external_addresses_rejected=2");
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.malformed_table_retry_is_complete_and_balanced",
    "[integration][xinput][dll][process]") {
    inputnext::xinput_dll_tests::temporary_directory directory{L"malformed-retry"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_4.dll");
    copy_external_callback_fixture(directory.path());
    const auto malformed = malformed_fixture_paths();
    const auto result = inputnext::xinput_dll_tests::run_probe({L"malformed-retry",
                                                                   directory.path().native(),
                                                                   malformed[0U].native(),
                                                                   malformed[1U].native(),
                                                                   malformed[2U].native(),
                                                                   malformed[3U].native(),
                                                                   table_fixture_path().native()},
        std::chrono::seconds{30},
        directory.path());
    require_child_success(result, "malformed_retry_cases=4");
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.missing_query_retry_releases_temporary_runtime",
    "[integration][xinput][dll][process]") {
    inputnext::xinput_dll_tests::temporary_directory directory{L"missing-query-retry"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_3.dll");
    copy_facade(bundle_directory(), directory.path(), L"xinput1_4.dll");
    inputnext::xinput_dll_tests::copy_fixture(missing_query_fixture_path(),
        directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(directory.path());

    const auto result =
        inputnext::xinput_dll_tests::run_probe({L"missing-query-retry",
                                                   directory.path().native(),
                                                   table_fixture_path().native(),
                                                   missing_query_fixture_path().native()},
            std::chrono::seconds{30},
            directory.path());
    require_child_success(result, "missing_query_retry=passed");
    check_temporary_directory_cleanup(directory);
}

TEST_CASE("xinput.dll.same_and_cross_facade_reentry_fail_closed",
    "[integration][xinput][dll][process]") {
    inputnext::xinput_dll_tests::temporary_directory same_directory{L"reentry-same"};
    copy_facade(bundle_directory(), same_directory.path(), L"xinput1_4.dll");
    copy_facade(bundle_directory(), same_directory.path(), L"xinput1_3.dll");
    inputnext::xinput_dll_tests::copy_fixture(table_fixture_path(),
        same_directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(same_directory.path());
    const auto same = run_isolated(L"reentry-same", same_directory.path());
    require_child_success(same, "reentry=same-failed-closed");

    inputnext::xinput_dll_tests::temporary_directory cross_directory{L"reentry-cross"};
    copy_facade(bundle_directory(), cross_directory.path(), L"xinput1_4.dll");
    copy_facade(bundle_directory(), cross_directory.path(), L"xinput1_3.dll");
    inputnext::xinput_dll_tests::copy_fixture(table_fixture_path(),
        cross_directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(cross_directory.path());
    const auto cross = run_isolated(L"reentry-cross", cross_directory.path());
    require_child_success(cross, "reentry=cross-failed-closed");
    check_temporary_directory_cleanup(same_directory);
    check_temporary_directory_cleanup(cross_directory);
}

TEST_CASE("xinput.dll.ar1_rejecting_pin_constructs_nothing_and_unloads",
    "[integration][xinput][dll][process][lifetime]") {
    inputnext::xinput_dll_tests::temporary_directory directory{L"ar1-rejection"};
    copy_facade(bundle_directory(), directory.path(), L"xinput1_4.dll");
    inputnext::xinput_dll_tests::copy_fixture(ar1_fixture_path(),
        directory.path() / L"inputnext_runtime.dll");
    copy_external_callback_fixture(directory.path());

    const auto result = inputnext::xinput_dll_tests::run_probe(
        {L"ar1-rejection", directory.path().native(), table_fixture_path().native()},
        std::chrono::seconds{30},
        directory.path());
    require_child_success(result, "ar1_unload_reload_cycles=3");
    check_temporary_directory_cleanup(directory);
}
