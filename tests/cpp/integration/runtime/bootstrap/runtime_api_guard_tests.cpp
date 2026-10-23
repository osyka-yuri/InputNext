#include "runtime/abi/runtime_api.hpp"

#include "runtime_api_test_composition.hpp"
#include <inputnext/xinput_types.hpp>

#include <Windows.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <thread>

namespace {

std::byte guide_context_sentinel{};

void check_rejected_arguments(const input_next_runtime_api_v1& api) {
    XINPUT_STATE state{};
    state.dwPacketNumber = 0x12345678U;
    state.Gamepad.wButtons = 0x5AA5U;
    CHECK(api.get_state(INPUT_NEXT_FLAVOR_1_3, 4U, &state) == ERROR_BAD_ARGUMENTS);
    CHECK(state.dwPacketNumber == 0x12345678U);
    CHECK(state.Gamepad.wButtons == 0x5AA5U);
    CHECK(api.get_state_ex(INPUT_NEXT_FLAVOR_1_4, 0U, nullptr) == ERROR_BAD_ARGUMENTS);
    CHECK(api.get_state(99U, 0U, &state) == ERROR_BAD_ARGUMENTS);
    CHECK(state.dwPacketNumber == 0x12345678U);
    CHECK(state.Gamepad.wButtons == 0x5AA5U);
    CHECK(api.get_state_ex(INPUT_NEXT_FLAVOR_1_4, 4U, &state) == ERROR_BAD_ARGUMENTS);
    CHECK(state.dwPacketNumber == 0x12345678U);
    CHECK(state.Gamepad.wButtons == 0x5AA5U);
    CHECK(api.set_state(INPUT_NEXT_FLAVOR_1_4, 0U, nullptr) == ERROR_BAD_ARGUMENTS);
    XINPUT_VIBRATION vibration{};
    CHECK(api.set_state(INPUT_NEXT_FLAVOR_1_3, 4U, &vibration) == ERROR_BAD_ARGUMENTS);

    XINPUT_CAPABILITIES capabilities{};
    capabilities.Type = 0xA5U;
    capabilities.Gamepad.wButtons = 0x1357U;
    CHECK(
        api.get_capabilities(INPUT_NEXT_FLAVOR_1_3, 0U, 2U, &capabilities) == ERROR_BAD_ARGUMENTS);
    CHECK(capabilities.Type == 0xA5U);
    CHECK(capabilities.Gamepad.wButtons == 0x1357U);
    CHECK(api.get_capabilities(INPUT_NEXT_FLAVOR_1_4, 4U, 0U, nullptr) == ERROR_BAD_ARGUMENTS);

    inputnext::xinput::capabilities_ex capabilities_ex{};
    capabilities_ex.vendor_id = 0xA5A5U;
    CHECK(api.get_capabilities_ex(INPUT_NEXT_FLAVOR_1_4, 0U, 2U, &capabilities_ex) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(capabilities_ex.vendor_id == 0xA5A5U);
    CHECK(api.get_capabilities_ex(INPUT_NEXT_FLAVOR_1_3, 4U, 0U, nullptr) == ERROR_BAD_ARGUMENTS);

    XINPUT_BATTERY_INFORMATION battery{};
    battery.BatteryType = 0xA5U;
    battery.BatteryLevel = 0x5AU;
    CHECK(api.get_battery_information(INPUT_NEXT_FLAVOR_1_4, 0U, 0xFFU, &battery) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(battery.BatteryType == 0xA5U);
    CHECK(battery.BatteryLevel == 0x5AU);
    CHECK(
        api.get_battery_information(INPUT_NEXT_FLAVOR_1_3, 0U, BATTERY_DEVTYPE_GAMEPAD, nullptr) ==
        ERROR_BAD_ARGUMENTS);

    XINPUT_KEYSTROKE keystroke{};
    keystroke.VirtualKey = 0xA55AU;
    CHECK(api.get_keystroke(INPUT_NEXT_FLAVOR_9_1_0, 4U, 0xFFFFFFFFU, nullptr) ==
          ERROR_NOT_SUPPORTED);
    CHECK(api.get_keystroke(INPUT_NEXT_FLAVOR_1_3, 4U, 0xFFFFFFFFU, &keystroke) ==
          ERROR_BAD_ARGUMENTS);
    CHECK(keystroke.VirtualKey == 0xA55AU);

    std::array<wchar_t, 2U> render_id{L'R', L'X'};
    std::array<wchar_t, 2U> capture_id{L'C', L'X'};
    UINT render_count = 0x1234U;
    UINT capture_count = 0x5678U;
    CHECK(api.get_audio_device_ids(INPUT_NEXT_FLAVOR_1_3,
              4U,
              render_id.data(),
              &render_count,
              capture_id.data(),
              &capture_count) == ERROR_NOT_SUPPORTED);
    CHECK(render_id[0U] == L'R');
    CHECK(capture_id[0U] == L'C');
    CHECK(render_count == 0x1234U);
    CHECK(capture_count == 0x5678U);
    CHECK(api.get_audio_device_ids(INPUT_NEXT_FLAVOR_1_4,
              4U,
              render_id.data(),
              &render_count,
              capture_id.data(),
              &capture_count) == ERROR_BAD_ARGUMENTS);
    CHECK(render_id[0U] == L'R');
    CHECK(capture_id[0U] == L'C');
    CHECK(render_count == 0x1234U);
    CHECK(capture_count == 0x5678U);
    CHECK(api.get_audio_device_ids(99U, 0U, nullptr, nullptr, nullptr, nullptr) ==
          ERROR_BAD_ARGUMENTS);

    GUID render_guid{};
    render_guid.Data1 = 0x12345678U;
    GUID capture_guid{};
    capture_guid.Data1 = 0x87654321U;
    CHECK(
        api.get_dsound_audio_device_guids(INPUT_NEXT_FLAVOR_1_4, 4U, &render_guid, &capture_guid) ==
        ERROR_NOT_SUPPORTED);
    CHECK(render_guid.Data1 == 0x12345678U);
    CHECK(capture_guid.Data1 == 0x87654321U);
    CHECK(
        api.get_dsound_audio_device_guids(INPUT_NEXT_FLAVOR_1_3, 4U, &render_guid, &capture_guid) ==
        ERROR_BAD_ARGUMENTS);
    CHECK(render_guid.Data1 == 0x12345678U);
    CHECK(capture_guid.Data1 == 0x87654321U);

    CHECK(api.wait_for_guide_button(INPUT_NEXT_FLAVOR_1_3,
              4U,
              0xFFFFFFFFU,
              &guide_context_sentinel) == ERROR_BAD_ARGUMENTS);
    CHECK(api.cancel_guide_button_wait(INPUT_NEXT_FLAVOR_1_4, 4U) == ERROR_BAD_ARGUMENTS);
    CHECK(api.power_off_controller(INPUT_NEXT_FLAVOR_1_3, 4U) == ERROR_BAD_ARGUMENTS);
}

void check_permissive_arguments(const input_next_runtime_api_v1& api) {
    XINPUT_KEYSTROKE keystroke{};
    CHECK(api.get_keystroke(INPUT_NEXT_FLAVOR_1_4, 0xFFU, 0xFFFFFFFFU, &keystroke) !=
          ERROR_BAD_ARGUMENTS);
    CHECK(api.get_audio_device_ids(INPUT_NEXT_FLAVOR_1_4, 0U, nullptr, nullptr, nullptr, nullptr) !=
          ERROR_BAD_ARGUMENTS);
    CHECK(api.get_dsound_audio_device_guids(INPUT_NEXT_FLAVOR_1_3, 0U, nullptr, nullptr) !=
          ERROR_BAD_ARGUMENTS);

    auto* const ignored_context = &guide_context_sentinel;
    const auto guide_status =
        api.wait_for_guide_button(INPUT_NEXT_FLAVOR_1_4, 0U, 0xFFFFFFFFU, ignored_context);
    CHECK((guide_status == ERROR_NOT_SUPPORTED || guide_status == ERROR_DEVICE_NOT_CONNECTED));
    CHECK(api.cancel_guide_button_wait(INPUT_NEXT_FLAVOR_1_3, 0U) != ERROR_BAD_ARGUMENTS);
    CHECK(api.power_off_controller(INPUT_NEXT_FLAVOR_1_4, 0U) != ERROR_BAD_ARGUMENTS);
}

} // namespace

TEST_CASE("runtime.abi.actual_adapter_guards_reject_arguments_before_cold_bootstrap",
    "[integration][runtime][bootstrap][abi]") {
    using namespace inputnext::runtime_detail;

    const runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    check_rejected_arguments(*api);
    CHECK(fixture.focus_observations() == 0U);
    CHECK(fixture.prepare_attempts() == 0U);
    CHECK_FALSE(fixture.native_initialized());
}

TEST_CASE("runtime.abi.actual_adapter_guards_remain_exact_when_ready",
    "[integration][runtime][bootstrap][abi]") {
    using namespace inputnext::runtime_detail;

    runtime_api_test_fixture fixture;
    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    XINPUT_STATE startup_state{};
    CHECK(api->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &startup_state) == ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(fixture.owner_is_ready());
    fixture.connect_native_device();

    XINPUT_STATE state{};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    bool connected{};
    while (std::chrono::steady_clock::now() < deadline) {
        if (api->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &state) == ERROR_SUCCESS) {
            connected = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    REQUIRE(connected);
    REQUIRE(fixture.owner_is_ready());

    check_rejected_arguments(*api);
    check_permissive_arguments(*api);
    CHECK(fixture.focus_observations() == 1U);
    CHECK(fixture.prepare_attempts() == 1U);
}

TEST_CASE("runtime.abi.actual_adapter_guards_remain_exact_when_degraded",
    "[integration][runtime][bootstrap][abi][failure]") {
    using namespace inputnext::runtime_detail;

    const runtime_api_test_fixture fixture{true};
    const auto* const api = input_next_runtime_query_api(1U);
    REQUIRE(api != nullptr);
    XINPUT_STATE state{};
    CHECK(api->get_state(INPUT_NEXT_FLAVOR_1_4, 0U, &state) == ERROR_DEVICE_NOT_CONNECTED);
    REQUIRE(fixture.owner_is_degraded());

    check_rejected_arguments(*api);
    check_permissive_arguments(*api);
    CHECK(fixture.focus_observations() == 1U);
    CHECK(fixture.prepare_attempts() == 1U);
}
