#include "dll_probe_support.hpp"

#include "table_fixture_protocol.hpp"

#include <cstdint>
#include <filesystem>
#include <iostream>

namespace inputnext::xinput_dll_probe {

[[nodiscard]] int run_production(const std::filesystem::path& bundle) {
    module_handle facade13 = load_library(bundle / L"xinput1_3.dll");
    module_handle facade14{LoadLibraryW(L"xinput1_4.dll")};
    module_handle facade910 = load_library(bundle / L"xinput9_1_0.dll");
    require(facade13.get() != nullptr && facade14.get() != nullptr && facade910.get() != nullptr,
        "could not load all three production facades");
    require(module_path_matches(facade14.get(), bundle / L"xinput1_4.dll"),
        "name-only load did not reuse the app-local companion");
    require(module_not_loaded() != 0U, "facade load initialized the runtime eagerly");
    require(runtime_module_count() == 0U, "runtime was present before a facade entry call");

    const DWORD first_result = call_valid_state_and_check_output(facade14.get());
    require(first_result == ERROR_SUCCESS || first_result == ERROR_DEVICE_NOT_CONNECTED,
        "production GetState returned an unexpected result");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "valid production GetState did not resolve the sibling runtime");
    static_cast<void>(call_invalid_state_and_check_output(facade14.get()));
    std::cout << "production_table_state=discovery-attempted\n";

    const auto runtime_path = bundle / L"inputnext_runtime.dll";
    auto sibling_runtime = load_library(runtime_path);
    require(sibling_runtime.get() != nullptr, "could not load the app-local runtime witness");
    require(module_path_matches(sibling_runtime.get(), runtime_path),
        "runtime witness did not resolve to the app-local sibling");
    require(GetModuleHandleW(L"inputnext_runtime.dll") == sibling_runtime.get(),
        "runtime witness was not the process runtime module");
    require(runtime_module_count() == 1U, "more than one runtime module is loaded");

    const auto require_shared_runtime = [&runtime_path, expected = sibling_runtime.get()] {
        HMODULE const loaded = GetModuleHandleW(L"inputnext_runtime.dll");
        require(loaded == expected, "facades did not share one runtime module handle");
        require(runtime_module_count() == 1U, "facades loaded more than one runtime module");
        require(module_path_matches(loaded, runtime_path),
            "a facade loaded a runtime outside its app-local sibling directory");
    };

    static_cast<void>(call_valid_state_and_check_output(facade13.get()));
    require_shared_runtime();
    static_cast<void>(call_valid_state_and_check_output(facade14.get()));
    require_shared_runtime();
    static_cast<void>(call_valid_state_and_check_output(facade910.get()));
    require_shared_runtime();

    require(sibling_runtime.close(), "could not release the runtime witness reference");
    require(facade13.close() && facade14.close() && facade910.close(),
        "could not quiescently unload the production facades");

    module_handle reloaded_facade = load_library(bundle / L"xinput1_4.dll");
    require(reloaded_facade.get() != nullptr, "could not reload a production facade");
    static_cast<void>(call_valid_state_and_check_output(reloaded_facade.get()));
    require(reloaded_facade.close(), "could not unload the reloaded facade");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "accepted production provider was not process-retained after facade reload");
    std::cout << "production_unload_reload=passed\n";
    return 0;
}

[[nodiscard]] int run_provider_lifetime(const std::filesystem::path& directory) {
    require(module_not_loaded() != 0U && runtime_module_count() == 0U,
        "provider lifetime scenario started with an external provider owner");

    module_handle facade = load_library(directory / L"xinput1_4.dll");
    require(facade.get() != nullptr, "could not load the no-preload facade");
    require(module_not_loaded() != 0U && runtime_module_count() == 0U,
        "facade load initialized the provider before the first entry call");

    XINPUT_STATE state{};
    require(call_get_state(facade.get(), 0U, &state) == expected_result(callback_id::get_state),
        "no-preload facade did not call the valid provider");

    HMODULE const provider = GetModuleHandleW(L"inputnext_runtime.dll");
    const auto provider_path = directory / L"inputnext_runtime.dll";
    require(provider != nullptr && module_path_matches(provider, provider_path),
        "facade did not resolve and pin the app-local provider");
    require(runtime_module_count() == 1U, "provider lifetime scenario loaded multiple providers");

    module_handle witness = load_library(provider_path);
    require(witness.get() == provider, "post-call provider witness resolved to another module");
    const auto get_snapshot =
        load_named_function<snapshot_fn>(witness.get(), "inputnext_fixture_get_snapshot");
    snapshot initial{};
    require(get_snapshot(&initial) != FALSE && initial.query_calls == 1U &&
                initial.callback_calls == 1U,
        "no-preload provider did not retain its table and callback after dispatch");
    require(witness.close(), "could not balance the provider witness reference");
    require(facade.close(), "could not quiescently unload the first facade");
    require(GetModuleHandleW(L"inputnext_runtime.dll") == provider && runtime_module_count() == 1U,
        "accepted provider did not remain process-retained after facade unload");

    constexpr std::uint32_t reload_count = 2U;
    for (std::uint32_t cycle = 0U; cycle < reload_count; ++cycle) {
        module_handle reloaded_facade = load_library(directory / L"xinput1_4.dll");
        require(reloaded_facade.get() != nullptr, "could not reload the provider facade");
        require(call_get_state(reloaded_facade.get(), 0U, &state) ==
                    expected_result(callback_id::get_state),
            "reloaded facade did not call the process-retained provider");

        snapshot reloaded{};
        require(get_snapshot(&reloaded) != FALSE && reloaded.query_calls == cycle + 2U &&
                    reloaded.callback_calls == cycle + 2U,
            "provider query or callback did not survive facade reload");
        require(reloaded_facade.close(), "could not quiescently unload a reloaded facade");
        require(GetModuleHandleW(L"inputnext_runtime.dll") == provider &&
                    runtime_module_count() == 1U,
            "facade reload changed the single retained provider image");
    }

    std::cout << "provider_lifetime=passed\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
