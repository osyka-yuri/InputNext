#include "dll_probe_support.hpp"

#include "ar1_fixture_protocol.hpp"
#include "dll_probe_argument_checks.hpp"
#include "table_fixture_protocol.hpp"

#include <Windows.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>

namespace inputnext::xinput_dll_probe {

[[nodiscard]] int run_malformed_retry(const std::filesystem::path& directory,
    const std::array<std::filesystem::path, 4U>& malformed_fixtures,
    const std::filesystem::path& valid_fixture) {
    require(module_not_loaded() != 0U && runtime_module_count() == 0U,
        "malformed retry started with a preloaded provider");

    module_handle facade = load_library(directory / L"xinput1_4.dll");
    require(facade.get() != nullptr, "could not load a facade for malformed-table retry");
    const auto get_state = load_named_function<get_state_fn>(facade.get(), "XInputGetState");
    for (const auto& malformed_fixture : malformed_fixtures) {
        const auto source = std::filesystem::absolute(malformed_fixture).wstring();
        const auto destination = extended_path(
            std::filesystem::absolute(directory / L"inputnext_runtime.dll").wstring());
        require(CopyFileW(source.c_str(), destination.c_str(), FALSE) != FALSE,
            "could not stage a malformed table provider");

        XINPUT_STATE state{};
        std::memset(&state, 0x6B, sizeof(state));
        XINPUT_STATE before{};
        std::memcpy(&before, &state, sizeof(before));
        require(get_state(0U, &state) == ERROR_DEVICE_NOT_CONNECTED,
            "facade accepted an incomplete or incompatible table");
        require(state_is_unchanged(state, before), "malformed-table fallback changed output");
        require(module_not_loaded() != 0U && runtime_module_count() == 0U,
            "malformed provider was retained before a valid table was accepted");
    }

    const auto valid_source = std::filesystem::absolute(valid_fixture).wstring();
    const auto destination =
        extended_path(std::filesystem::absolute(directory / L"inputnext_runtime.dll").wstring());
    require(CopyFileW(valid_source.c_str(), destination.c_str(), FALSE) != FALSE,
        "could not stage the valid table provider for retry");

    XINPUT_STATE state{};
    require(get_state(0U, &state) == expected_result(callback_id::get_state),
        "facade did not retry after malformed-table providers unloaded");
    HMODULE const provider = GetModuleHandleW(L"inputnext_runtime.dll");
    require(provider != nullptr && runtime_module_count() == 1U,
        "facade did not retain exactly one provider after accepting a valid table");
    module_handle witness = load_library(directory / L"inputnext_runtime.dll");
    require(witness.get() == provider, "valid provider witness resolved to another module");
    const auto get_snapshot =
        load_named_function<snapshot_fn>(witness.get(), "inputnext_fixture_get_snapshot");
    snapshot retried{};
    require(get_snapshot(&retried) != FALSE && retried.query_calls == 1U &&
                retried.callback_calls == 1U,
        "valid retry did not query and dispatch exactly once");
    require(witness.close(), "could not balance the valid-provider witness reference");
    require(facade.close(), "could not quiescently unload the retried facade");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "accepted provider did not remain process-retained after retry");

    std::cout << "malformed_retry_cases=4\n";
    return 0;
}

[[nodiscard]] int run_missing_query_retry(const missing_query_retry_request& request) {
    const auto& directory = request.directory;
    const auto& valid_fixture = request.valid_fixture;
    const auto valid_source = std::filesystem::absolute(valid_fixture).wstring();
    const auto missing_query_source =
        std::filesystem::absolute(request.missing_query_fixture).wstring();
    const auto destination =
        extended_path(std::filesystem::absolute(directory / L"inputnext_runtime.dll").wstring());
    require(module_not_loaded() != 0U && runtime_module_count() == 0U,
        "missing-query matrix started with a preloaded runtime provider");
    require(CopyFileW(valid_source.c_str(), destination.c_str(), FALSE) != FALSE,
        "could not stage the temporary table provider for argument guards");

    {
        module_handle guard_runtime = load_library(directory / L"inputnext_runtime.dll");
        require(guard_runtime.get() != nullptr,
            "could not load a query-counting provider for the argument matrix");
        reset_fixture(guard_runtime.get());
        require(configure_fixture(guard_runtime.get(), query_mode::valid),
            "could not configure the query-counting provider for the argument matrix");
        module_handle facade14 = load_library(directory / L"xinput1_4.dll");
        module_handle facade13 = load_library(directory / L"xinput1_3.dll");
        require(facade14.get() != nullptr && facade13.get() != nullptr,
            "could not load both fixed-flavor argument-guard facades");
        const auto get_guard_snapshot =
            load_named_function<snapshot_fn>(guard_runtime.get(), "inputnext_fixture_get_snapshot");
        check_guarded_calls(facade14.get(), facade13.get());
        snapshot guarded{};
        require(get_guard_snapshot(&guarded) != FALSE && guarded.query_calls == 0U &&
                    guarded.callback_calls == 0U,
            "invalid per-function calls performed provider discovery or callback dispatch");
        require(facade14.close() && facade13.close(),
            "could not close the argument-guard facades before provider replacement");
        require(guard_runtime.close(),
            "could not balance the query-counting provider inspection reference");
    }
    require(module_not_loaded() != 0U && runtime_module_count() == 0U,
        "invalid argument calls retained or pinned their query-counting provider");
    require(CopyFileW(missing_query_source.c_str(), destination.c_str(), FALSE) != FALSE,
        "could not stage the missing-query provider after argument-guard checks");

    module_handle facade = load_library(directory / L"xinput1_4.dll");
    module_handle facade13 = load_library(directory / L"xinput1_3.dll");
    require(facade.get() != nullptr && facade13.get() != nullptr,
        "could not load both facades for missing-query argument checks");
    check_guarded_calls(facade.get(), facade13.get());
    require(module_not_loaded() != 0U && runtime_module_count() == 0U,
        "invalid arguments loaded or retained the installed missing-query runtime");
    XINPUT_STATE state{};
    std::memset(&state, 0x7C, sizeof(state));
    XINPUT_STATE before{};
    std::memcpy(&before, &state, sizeof(before));
    require(call_get_state(facade.get(), 0U, &state) == ERROR_DEVICE_NOT_CONNECTED,
        "missing query export did not fail closed");
    require(state_is_unchanged(state, before), "missing-query fallback changed output");

    const auto wait_for_guide =
        load_ordinal_function<DWORD(WINAPI*)(DWORD, DWORD, LPVOID) noexcept>(facade.get(), 101U);
    std::uintptr_t invalid_context_storage{};
    LPVOID const invalid_context = &invalid_context_storage;
    require(wait_for_guide(XUSER_MAX_COUNT, 0xFFFF'FFFFU, invalid_context) == ERROR_BAD_ARGUMENTS,
        "Guide wait invalid-index fallback did not take precedence");
    require(wait_for_guide(0U, 0xFFFF'FFFFU, invalid_context) == ERROR_NOT_SUPPORTED,
        "Guide wait connected-index fallback was not NOT_SUPPORTED");
    const auto cancel_guide =
        load_ordinal_function<DWORD(WINAPI*)(DWORD) noexcept>(facade.get(), 102U);
    require(cancel_guide(XUSER_MAX_COUNT) == ERROR_BAD_ARGUMENTS &&
                cancel_guide(0U) == ERROR_NOT_SUPPORTED,
        "Guide cancellation fallback did not preserve index precedence");
    const auto power_off =
        load_ordinal_function<DWORD(WINAPI*)(DWORD) noexcept>(facade.get(), 103U);
    require(power_off(XUSER_MAX_COUNT) == ERROR_BAD_ARGUMENTS &&
                power_off(0U) == ERROR_NOT_SUPPORTED,
        "power-off fallback did not preserve index precedence");
    require(module_not_loaded() != 0U,
        "missing-query failure retained its temporary runtime module reference");

    require(CopyFileW(valid_source.c_str(), destination.c_str(), FALSE) != FALSE,
        "missing-query runtime could not be replaced after resolver failure");

    module_handle runtime = load_library(directory / L"inputnext_runtime.dll");
    require(runtime.get() != nullptr, "could not load replacement table fixture");
    reset_fixture(runtime.get());
    require(configure_fixture(runtime.get(), query_mode::valid),
        "could not configure replacement fixture");
    const auto get_state = load_named_function<get_state_fn>(facade.get(), "XInputGetState");
    require(get_state(0U, &state) == expected_result(callback_id::get_state),
        "facade did not retry after the query export became available");
    const auto get_snapshot =
        load_named_function<snapshot_fn>(runtime.get(), "inputnext_fixture_get_snapshot");
    require(ensure_query_count(get_snapshot, 1U) == 1U,
        "replacement query did not receive ABI version 1");

    check_permissive_calls(facade.get(), get_snapshot);
    snapshot after_14{};
    require(get_snapshot(&after_14) != FALSE && after_14.query_calls == 1U &&
                after_14.callback_calls == 6U,
        "permissive 1.4 calls did not reuse the accepted provider table");
    check_permissive_dsound_call(facade13.get(), get_snapshot);
    snapshot completed{};
    require(get_snapshot(&completed) != FALSE && completed.query_calls >= 1U &&
                completed.query_calls <= 2U && completed.callback_calls == 7U,
        "permissive argument checks did not dispatch through the retried provider");

    require(facade.close() && facade13.close(), "could not quiescently unload the retried facades");
    require(runtime.close(), "could not release the replacement fixture module");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "accepted provider was not retained after balancing temporary references");
    std::cout << "missing_query_retry=passed\n";
    return 0;
}

[[nodiscard]] int run_reentry(const std::filesystem::path& directory, const bool cross_facade) {
    module_handle runtime = load_library(directory / L"inputnext_runtime.dll");
    require(runtime.get() != nullptr, "could not load the reentry table fixture");
    module_handle facade14 = load_library(directory / L"xinput1_4.dll");
    module_handle facade13;
    if (cross_facade) {
        facade13 = load_library(directory / L"xinput1_3.dll");
        require(facade13.get() != nullptr, "could not load the cross-reentry facade");
    }
    require(facade14.get() != nullptr, "could not load the reentry facade");

    reset_fixture(runtime.get());
    HMODULE const target = cross_facade ? facade13.get() : facade14.get();
    const auto mode =
        cross_facade ? query_mode::reenter_cross_facade : query_mode::reenter_same_facade;
    require(configure_fixture(runtime.get(), mode, target), "could not configure reentry fixture");
    XINPUT_STATE state{};
    std::memset(&state, 0x54, sizeof(state));
    XINPUT_STATE before{};
    std::memcpy(&before, &state, sizeof(before));
    const auto result = call_get_state(facade14.get(), 0U, &state);
    require(state_is_unchanged(state, before), "reentrant outer call changed its output");
    require(result == expected_result(callback_id::get_state),
        "outer facade call did not continue after recursive entry failed closed");
    const auto get_snapshot =
        load_named_function<snapshot_fn>(runtime.get(), "inputnext_fixture_get_snapshot");
    expect_reentry_result(get_snapshot, cross_facade ? 2U : 1U, callback_id::get_state);

    if (cross_facade) {
        require(facade13.close(), "could not quiescently unload the cross-reentry facade");
    }
    require(facade14.close(), "could not quiescently unload the reentry facade");
    require(runtime.close(), "could not release the reentry fixture");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "accepted reentry provider was not process-retained after facade unload");
    std::cout << (cross_facade ? "reentry=cross-failed-closed\n" : "reentry=same-failed-closed\n");
    return 0;
}

[[nodiscard]] int run_ar1_rejection(const ar1_rejection_request& request) {
    const auto& directory = request.directory;
    const auto& valid_fixture = request.valid_fixture;
    constexpr std::uint32_t expected_cycles = 3U;
    for (std::uint32_t cycle = 0U; cycle < expected_cycles; ++cycle) {
        module_handle runtime = load_library(directory / L"inputnext_runtime.dll");
        require(runtime.get() != nullptr, "could not load the AR1 runtime fixture");
        const auto reset =
            load_named_function<ar1_reset_fn>(runtime.get(), "inputnext_ar1_reset_snapshot");
        const auto get_snapshot =
            load_named_function<ar1_snapshot_fn>(runtime.get(), "inputnext_ar1_get_snapshot");
        reset();

        module_handle facade = load_library(directory / L"xinput1_4.dll");
        require(facade.get() != nullptr, "could not load the AR1 facade");
        XINPUT_STATE state{};
        std::memset(&state, 0x3D, sizeof(state));
        XINPUT_STATE before{};
        std::memcpy(&before, &state, sizeof(before));
        require(call_get_state(facade.get(), 0U, &state) == ERROR_DEVICE_NOT_CONNECTED,
            "rejecting pin did not make the facade fail closed");
        require(state_is_unchanged(state, before), "rejecting-pin fallback changed output");

        ar1_fixture_snapshot counters{};
        require(get_snapshot(&counters) != FALSE, "AR1 fixture snapshot failed");
        require(counters.pin_attempts == 1U && counters.owner_factory_calls == 0U &&
                    counters.stable_api_calls == 0U && counters.dependency_constructions == 0U &&
                    counters.graph_constructions == 0U && counters.native_starts == 0U &&
                    counters.operator_new_calls == 0U,
            "AR1 rejecting-pin path created owner, dependency, graph, native, or heap state");

        require(facade.close(), "could not quiescently unload the AR1 facade");
        require(runtime.close(), "could not release the rejecting-pin runtime fixture");
        require(module_not_loaded() != 0U,
            "rejecting-pin cycle retained the temporary runtime module reference");
    }

    const auto valid_source = std::filesystem::absolute(valid_fixture).wstring();
    const auto destination =
        extended_path(std::filesystem::absolute(directory / L"inputnext_runtime.dll").wstring());
    require(CopyFileW(valid_source.c_str(), destination.c_str(), FALSE) != FALSE,
        "could not stage a valid provider after rejected pin attempts");
    module_handle accepted_facade = load_library(directory / L"xinput1_4.dll");
    require(accepted_facade.get() != nullptr,
        "could not load a facade after rejected pin attempts");
    XINPUT_STATE accepted_state{};
    require(call_get_state(accepted_facade.get(), 0U, &accepted_state) ==
                expected_result(callback_id::get_state),
        "facade did not accept a valid provider after rejected pin attempts");
    require(accepted_facade.close(), "could not quiescently unload the accepted facade");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "accepted provider was not retained after rejected pin references were balanced");

    std::cout << "ar1_unload_reload_cycles=3\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
