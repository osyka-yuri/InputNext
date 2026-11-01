#include "support.hpp"

#include "fixtures/table_fixture_protocol.hpp"
#include "runtime/abi/xinput/api.hpp"

#include <Windows.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>

namespace inputnext::xinput_dll_probe {
namespace {

template <typename Address>
[[nodiscard]] HMODULE module_for_address(const Address address) {
    HMODULE module{};
    constexpr DWORD flags =
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    require(GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(address), &module) != FALSE,
        "could not identify the module owning an external fixture address");
    return module;
}

template <typename Function>
void require_provider_callback(const HMODULE provider,
    const Function actual,
    const Function expected,
    const char* const name) {
    require(actual != nullptr, std::string{name} + " callback is null");
    require(actual == expected, std::string{name} + " callback differs from provider table");
    require(module_for_address(actual) == provider,
        std::string{name} + " callback is outside the provider image");
}

struct provider_images final {
    HMODULE runtime;
    HMODULE external_fixture;
};

void require_foreign_table_topology(const provider_images& images,
    const input_next_runtime_api_v1* const foreign_api,
    const input_next_runtime_api_v1* const provider_api) {
    const HMODULE runtime = images.runtime;
    const HMODULE external_fixture = images.external_fixture;
    if (foreign_api == nullptr || provider_api == nullptr) {
        throw std::runtime_error{"foreign-table fixture did not expose both API tables"};
    }
    require(foreign_api != provider_api, "foreign table unexpectedly aliases provider storage");
    require(is_valid_input_next_runtime_api_v1(foreign_api),
        "foreign table does not have the complete valid API shape");
    require(is_valid_input_next_runtime_api_v1(provider_api),
        "provider source table is not complete");
    require(module_for_address(foreign_api) == external_fixture,
        "foreign table storage is not owned by the external fixture image");

    require_provider_callback(runtime, foreign_api->get_state, provider_api->get_state, "GetState");
    require_provider_callback(runtime, foreign_api->set_state, provider_api->set_state, "SetState");
    require_provider_callback(runtime,
        foreign_api->get_capabilities,
        provider_api->get_capabilities,
        "GetCapabilities");
    require_provider_callback(runtime, foreign_api->enable, provider_api->enable, "Enable");
    require_provider_callback(runtime,
        foreign_api->get_battery_information,
        provider_api->get_battery_information,
        "GetBatteryInformation");
    require_provider_callback(runtime,
        foreign_api->get_keystroke,
        provider_api->get_keystroke,
        "GetKeystroke");
    require_provider_callback(runtime,
        foreign_api->get_audio_device_ids,
        provider_api->get_audio_device_ids,
        "GetAudioDeviceIds");
    require_provider_callback(runtime,
        foreign_api->get_dsound_audio_device_guids,
        provider_api->get_dsound_audio_device_guids,
        "GetDSoundAudioDeviceGuids");
    require_provider_callback(runtime,
        foreign_api->get_state_ex,
        provider_api->get_state_ex,
        "GetStateEx");
    require_provider_callback(runtime,
        foreign_api->get_capabilities_ex,
        provider_api->get_capabilities_ex,
        "GetCapabilitiesEx");
    require_provider_callback(runtime,
        foreign_api->wait_for_guide_button,
        provider_api->wait_for_guide_button,
        "WaitForGuideButton");
    require_provider_callback(runtime,
        foreign_api->cancel_guide_button_wait,
        provider_api->cancel_guide_button_wait,
        "CancelGuideButtonWait");
    require_provider_callback(runtime,
        foreign_api->power_off_controller,
        provider_api->power_off_controller,
        "PowerOffController");
}

} // namespace

[[nodiscard]] int run_external_addresses(const std::filesystem::path& directory) {
    constexpr std::array<query_mode, 2U> modes{query_mode::external_table,
        query_mode::external_callback};
    for (const auto mode : modes) {
        module_handle runtime = load_library(directory / L"inputnext_runtime.dll");
        require(runtime.get() != nullptr, "could not load the external-address table fixture");
        module_handle external_fixture =
            load_library(directory / L"inputnext_external_callback.dll");
        require(external_fixture.get() != nullptr, "could not load the external callback fixture");
        if (mode == query_mode::external_table) {
            const auto get_provider_api =
                load_named_function<api_table_fn>(runtime.get(), "inputnext_fixture_get_api");
            const auto copy_foreign_api =
                load_named_function<BOOL(__cdecl*)(const input_next_runtime_api_v1*) noexcept>(
                    external_fixture.get(),
                    "inputnext_external_copy_api");
            const auto get_foreign_api = load_named_function<api_table_fn>(external_fixture.get(),
                "inputnext_external_get_api");
            const auto* const provider_api = get_provider_api();
            require(copy_foreign_api(provider_api) != FALSE,
                "external fixture could not copy the complete provider API table");
            require_foreign_table_topology(
                {.runtime = runtime.get(), .external_fixture = external_fixture.get()},
                get_foreign_api(),
                provider_api);
        }
        module_handle facade = load_library(directory / L"xinput1_4.dll");
        require(facade.get() != nullptr, "could not load a facade for external-address rejection");
        const HMODULE target =
            mode == query_mode::external_table ? external_fixture.get() : facade.get();
        require(configure_fixture(runtime.get(), mode, target),
            "could not configure the external-address provider case");
        XINPUT_STATE state{};
        std::memset(&state, 0x2BU, sizeof(state));
        XINPUT_STATE before{};
        std::memcpy(&before, &state, sizeof(before));
        require(call_get_state(facade.get(), 0U, &state) == ERROR_DEVICE_NOT_CONNECTED,
            "facade accepted a table or callback outside the provider image");
        require(state_is_unchanged(state, before),
            "external-address rejection changed the untouched error output");

        const auto get_snapshot =
            load_named_function<snapshot_fn>(runtime.get(), "inputnext_fixture_get_snapshot");
        snapshot rejected{};
        require(get_snapshot(&rejected) != FALSE && rejected.query_calls == 1U &&
                    rejected.callback_calls == 0U,
            "external-address rejection reached a provider callback");
        require(facade.close(), "could not quiescently unload an external-address facade");
        require(runtime.close(), "could not release the rejected provider reference");
        require(external_fixture.close(), "could not release the external callback fixture");
        require(module_not_loaded() != 0U && runtime_module_count() == 0U &&
                    GetModuleHandleW(L"inputnext_external_callback.dll") == nullptr,
            "rejected provider stayed loaded after all temporary references were balanced");
    }

    std::cout << "external_addresses_rejected=2\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
