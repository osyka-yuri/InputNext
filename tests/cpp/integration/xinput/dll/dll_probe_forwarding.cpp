#include "dll_probe_support.hpp"

#include "table_fixture_protocol.hpp"
#include <inputnext/xinput_types.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>

namespace inputnext::xinput_dll_probe {

[[nodiscard]] int run_table_dispatch(const std::filesystem::path& directory) {
    module_handle runtime = load_library(directory / L"inputnext_runtime.dll");
    require(runtime.get() != nullptr, "could not load the table fixture");
    reset_fixture(runtime.get());
    require(configure_fixture(runtime.get(), query_mode::valid),
        "could not configure table fixture");

    module_handle facade14 = load_library(directory / L"xinput1_4.dll");
    require(facade14.get() != nullptr, "could not load the 1.4 fixture facade");

    const auto get_state = load_named_function<get_state_fn>(facade14.get(), "XInputGetState");
    XINPUT_STATE state{};
    require(get_state(0U, &state) == expected_result(callback_id::get_state),
        "GetState did not return the fixture callback result");
    auto get_snapshot =
        load_named_function<snapshot_fn>(runtime.get(), "inputnext_fixture_get_snapshot");
    expect_last_callback(get_snapshot,
        callback_id::get_state,
        1U,
        {0U, reinterpret_cast<std::uintptr_t>(&state)});

    const auto set_state =
        load_named_function<DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*) noexcept>(facade14.get(),
            "XInputSetState");
    XINPUT_VIBRATION vibration{.wLeftMotorSpeed = 0x1111U, .wRightMotorSpeed = 0x2222U};
    require(set_state(1U, &vibration) == expected_result(callback_id::set_state),
        "SetState did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::set_state,
        1U,
        {1U, reinterpret_cast<std::uintptr_t>(&vibration)});

    const auto get_capabilities =
        load_named_function<DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*) noexcept>(
            facade14.get(),
            "XInputGetCapabilities");
    XINPUT_CAPABILITIES capabilities{};
    require(get_capabilities(2U, XINPUT_FLAG_GAMEPAD, &capabilities) ==
                expected_result(callback_id::get_capabilities),
        "GetCapabilities did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_capabilities,
        1U,
        {2U, XINPUT_FLAG_GAMEPAD, reinterpret_cast<std::uintptr_t>(&capabilities)});

    const auto enable =
        load_named_function<void(WINAPI*)(BOOL) noexcept>(facade14.get(), "XInputEnable");
    enable(static_cast<BOOL>(0x41));
    expect_last_callback(get_snapshot, callback_id::enable, 1U, {0x41U});

    const auto get_battery =
        load_named_function<DWORD(WINAPI*)(DWORD, BYTE, XINPUT_BATTERY_INFORMATION*) noexcept>(
            facade14.get(),
            "XInputGetBatteryInformation");
    XINPUT_BATTERY_INFORMATION battery{};
    require(get_battery(3U, BATTERY_DEVTYPE_GAMEPAD, &battery) ==
                expected_result(callback_id::get_battery_information),
        "GetBatteryInformation did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_battery_information,
        1U,
        {3U, BATTERY_DEVTYPE_GAMEPAD, reinterpret_cast<std::uintptr_t>(&battery)});

    const auto get_keystroke =
        load_named_function<DWORD(WINAPI*)(DWORD, DWORD, PXINPUT_KEYSTROKE) noexcept>(
            facade14.get(),
            "XInputGetKeystroke");
    XINPUT_KEYSTROKE keystroke{};
    require(get_keystroke(0xFFU, 0x5151U, &keystroke) ==
                expected_result(callback_id::get_keystroke),
        "GetKeystroke did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_keystroke,
        1U,
        {0xFFU, 0x5151U, reinterpret_cast<std::uintptr_t>(&keystroke)});

    const auto get_audio =
        load_named_function<DWORD(WINAPI*)(DWORD, LPWSTR, UINT*, LPWSTR, UINT*) noexcept>(
            facade14.get(),
            "XInputGetAudioDeviceIds");
    std::array<wchar_t, 8U> render_id{};
    std::array<wchar_t, 8U> capture_id{};
    UINT render_count = static_cast<UINT>(render_id.size());
    UINT capture_count = static_cast<UINT>(capture_id.size());
    require(get_audio(0U, render_id.data(), &render_count, capture_id.data(), &capture_count) ==
                expected_result(callback_id::get_audio_device_ids),
        "GetAudioDeviceIds did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_audio_device_ids,
        1U,
        {0U,
            reinterpret_cast<std::uintptr_t>(render_id.data()),
            reinterpret_cast<std::uintptr_t>(&render_count),
            reinterpret_cast<std::uintptr_t>(capture_id.data()),
            reinterpret_cast<std::uintptr_t>(&capture_count)});

    const auto get_state_ex =
        load_ordinal_function<DWORD(WINAPI*)(DWORD, XINPUT_STATE*) noexcept>(facade14.get(), 100U);
    require(get_state_ex(1U, &state) == expected_result(callback_id::get_state_ex),
        "GetStateEx did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_state_ex,
        1U,
        {1U, reinterpret_cast<std::uintptr_t>(&state)});

    const auto get_capabilities_ex = load_ordinal_function<DWORD(
        WINAPI*)(DWORD, DWORD, inputnext::xinput::capabilities_ex*) noexcept>(facade14.get(), 108U);
    inputnext::xinput::capabilities_ex capabilities_ex{};
    require(get_capabilities_ex(2U, XINPUT_FLAG_GAMEPAD, &capabilities_ex) ==
                expected_result(callback_id::get_capabilities_ex),
        "GetCapabilitiesEx did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_capabilities_ex,
        1U,
        {2U, XINPUT_FLAG_GAMEPAD, reinterpret_cast<std::uintptr_t>(&capabilities_ex)});

    const auto wait_for_guide =
        load_ordinal_function<DWORD(WINAPI*)(DWORD, DWORD, LPVOID) noexcept>(facade14.get(), 101U);
    std::uintptr_t context_sentinel{};
    LPVOID const context = &context_sentinel;
    require(wait_for_guide(3U, 0x9191U, context) ==
                expected_result(callback_id::wait_for_guide_button),
        "Guide wait did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::wait_for_guide_button,
        1U,
        {3U, 0x9191U, reinterpret_cast<std::uintptr_t>(context)});

    const auto cancel_guide =
        load_ordinal_function<DWORD(WINAPI*)(DWORD) noexcept>(facade14.get(), 102U);
    require(cancel_guide(0U) == expected_result(callback_id::cancel_guide_button_wait),
        "Guide wait cancellation did not return the fixture callback result");
    expect_last_callback(get_snapshot, callback_id::cancel_guide_button_wait, 1U, {0U});

    const auto power_off =
        load_ordinal_function<DWORD(WINAPI*)(DWORD) noexcept>(facade14.get(), 103U);
    require(power_off(1U) == expected_result(callback_id::power_off_controller),
        "power-off did not return the fixture callback result");
    expect_last_callback(get_snapshot, callback_id::power_off_controller, 1U, {1U});
    require(ensure_query_count(get_snapshot, 1U) == 1U,
        "facade did not request ABI version 1 exactly once");

    require(facade14.close(), "could not quiescently unload the 1.4 fixture facade");
    reset_fixture(runtime.get());
    require(configure_fixture(runtime.get(), query_mode::valid), "could not reset table fixture");

    module_handle facade13 = load_library(directory / L"xinput1_3.dll");
    require(facade13.get() != nullptr, "could not load the 1.3 fixture facade");
    get_snapshot =
        load_named_function<snapshot_fn>(runtime.get(), "inputnext_fixture_get_snapshot");
    const auto get_dsound =
        load_named_function<DWORD(WINAPI*)(DWORD, GUID*, GUID*) noexcept>(facade13.get(),
            "XInputGetDSoundAudioDeviceGuids");
    GUID render_guid{};
    GUID capture_guid{};
    require(get_dsound(2U, &render_guid, &capture_guid) ==
                expected_result(callback_id::get_dsound_audio_device_guids),
        "GetDSoundAudioDeviceGuids did not return the fixture callback result");
    expect_last_callback(get_snapshot,
        callback_id::get_dsound_audio_device_guids,
        0U,
        {2U,
            reinterpret_cast<std::uintptr_t>(&render_guid),
            reinterpret_cast<std::uintptr_t>(&capture_guid)});
    require(ensure_query_count(get_snapshot, 1U) == 1U,
        "1.3 facade did not request ABI version 1 exactly once");
    require(facade13.close(), "could not quiescently unload the 1.3 fixture facade");
    require(runtime.close(), "could not release the table fixture module");
    require(module_not_loaded() == 0U && runtime_module_count() == 1U,
        "accepted table provider was not process-retained after facade unload");

    std::cout << "fixture_callbacks=13\n";
    return 0;
}

} // namespace inputnext::xinput_dll_probe
