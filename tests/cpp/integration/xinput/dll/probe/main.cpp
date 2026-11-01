#include "support.hpp"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <stdexcept>
#include <string_view>

int wmain(const int argc, wchar_t** const argv) {
    using namespace inputnext::xinput_dll_probe;
    try {
        require(argc >= 3, "expected a scenario and a test directory");
        const std::wstring_view scenario{argv[1]};
        const std::filesystem::path directory{argv[2]};
        if (scenario == L"production") {
            return run_production(directory);
        }
        if (scenario == L"companion") {
            return run_companion(directory);
        }
        if (scenario == L"companion-preloaded") {
            return run_companion_preloaded(directory);
        }
        if (scenario == L"companion-missing") {
            return run_companion_missing(directory);
        }
        if (scenario == L"companion-system-path") {
            return run_companion_system_path(directory);
        }
        if (scenario == L"core-query") {
            return run_core_query(directory);
        }
        if (scenario == L"provider-lifetime") {
            return run_provider_lifetime(directory);
        }
        if (scenario == L"external-addresses") {
            return run_external_addresses(directory);
        }
        if (scenario == L"table-dispatch") {
            return run_table_dispatch(directory);
        }
        if (scenario == L"malformed-retry") {
            require(argc >= 8, "missing malformed and valid table fixture paths");
            return run_malformed_retry(directory,
                {std::filesystem::path{argv[3]},
                    std::filesystem::path{argv[4]},
                    std::filesystem::path{argv[5]},
                    std::filesystem::path{argv[6]}},
                std::filesystem::path{argv[7]});
        }
        if (scenario == L"missing-query-retry") {
            require(argc >= 5, "missing table or missing-query fixture path");
            return run_missing_query_retry({.directory = directory,
                .valid_fixture = std::filesystem::path{argv[3]},
                .missing_query_fixture = std::filesystem::path{argv[4]}});
        }
        if (scenario == L"reentry-same") {
            return run_reentry(directory, false);
        }
        if (scenario == L"reentry-cross") {
            return run_reentry(directory, true);
        }
        if (scenario == L"ar1-rejection") {
            require(argc >= 4, "missing valid table fixture path");
            return run_ar1_rejection(
                {.directory = directory, .valid_fixture = std::filesystem::path{argv[3]}});
        }
        throw std::runtime_error("unknown XInput DLL probe scenario");
    } catch (const std::exception& error) {
        static_cast<void>(std::fprintf(stderr, "xinput_dll_probe_error=%s\n", error.what()));
        return 2;
    } catch (...) {
        static_cast<void>(std::fputs("xinput_dll_probe_error=unknown\n", stderr));
        return 3;
    }
}
