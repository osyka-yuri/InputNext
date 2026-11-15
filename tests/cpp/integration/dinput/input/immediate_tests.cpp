#include "core/core.hpp"
#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "cpp/integration/gameinput/runtime/runtime_test_support.hpp"
#include "dinput/input_access.hpp"
#include "dinput/property_abi.h"
#include "dinput/system_input.hpp"
#include "gameinput/native/native_gameinput.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dinput.h>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/xinput.hpp>
#include <inputnext/xinput_types.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

using namespace inputnext;
using namespace inputnext::dinput_test_support;

namespace {

[[nodiscard]] constexpr DWORD axis_x_id() noexcept {
    return DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U);
}

[[nodiscard]] DIPROPRANGE x_range_property() noexcept {
    DIPROPRANGE value{};
    value.diph.dwSize = data_size<DIPROPRANGE>();
    value.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    value.diph.dwObj = axis_x_id();
    value.diph.dwHow = DIPH_BYID;
    return value;
}

[[nodiscard]] DIPROPDWORD x_dword_property(DWORD data = 0U) noexcept {
    DIPROPDWORD property{};
    property.diph.dwSize = data_size<DIPROPDWORD>();
    property.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    property.diph.dwObj = axis_x_id();
    property.diph.dwHow = DIPH_BYID;
    property.dwData = data;
    return property;
}

[[nodiscard]] DIPROPDWORD device_dword_property(DWORD data = 0U) noexcept {
    DIPROPDWORD property{};
    property.diph.dwSize = data_size<DIPROPDWORD>();
    property.diph.dwHeaderSize = data_size<DIPROPHEADER>();
    property.diph.dwObj = 0U;
    property.diph.dwHow = DIPH_DEVICE;
    property.dwData = data;
    return property;
}

BOOL CALLBACK count_effect_callback(const DIEFFECTINFOW* const info, void* const context) {
    static_cast<void>(info);
    ++*static_cast<DWORD*>(context);
    return DIENUM_CONTINUE;
}

} // namespace

TEST_CASE("dinput.device_returns_standard_state_and_projects_object_properties",
    "[integration][dinput][input][immediate][properties]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const auto expected = sample_state();
    publish_gamepad(owners, 0U, expected);
    REQUIRE(wait_for_sample(*owners.core, target, expected));

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent);
    const auto* standard_format = dinput::system_joystick_format();
    REQUIRE(standard_format != nullptr);
    REQUIRE(device->SetDataFormat(standard_format) == DI_OK);
    const hidden_window window;
    REQUIRE(
        device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) == DI_OK);

    DIJOYSTATE state{};
    std::memset(&state, 0xA5, sizeof(state));
    const auto sentinel = state;
    CHECK(device->GetDeviceState(data_size<DIJOYSTATE>(), &state) == DIERR_NOTACQUIRED);
    CHECK(std::memcmp(&state, &sentinel, sizeof(state)) == 0);
    REQUIRE(device->Acquire() == DI_OK);
    CHECK(device->Acquire() == S_FALSE);
    CHECK(device->Poll() == DI_NOEFFECT);
    REQUIRE(device->GetDeviceState(data_size<DIJOYSTATE>(), &state) == DI_OK);

    CHECK(state.lX == 40959L);
    CHECK(state.lY == 40959L);
    CHECK(state.lZ == 16384L);
    CHECK(state.lRx == 24575L);
    CHECK(state.lRy == 24575L);
    CHECK(state.lRz == 49151L);
    CHECK(state.rgbButtons[0U] == 0x80U);
    CHECK(state.rgbButtons[1U] == 0x80U);
    CHECK(state.rgbButtons[2U] == 0U);
    CHECK(state.rgdwPOV[0U] == 4500U);

    REQUIRE(device->Unacquire() == DI_OK);
    REQUIRE(device->SetDataFormat(&c_dfDIJoystick2) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);
    DIJOYSTATE2 extended_state{};
    REQUIRE(device->GetDeviceState(data_size<DIJOYSTATE2>(), &extended_state) == DI_OK);
    CHECK(extended_state.lX == 40959L);
    CHECK(extended_state.lY == 40959L);
    CHECK(extended_state.lZ == 16384L);
    CHECK(extended_state.lRx == 24575L);
    CHECK(extended_state.rgbButtons[0U] == 0x80U);
    CHECK(extended_state.rgdwPOV[0U] == 4500U);
    CHECK(extended_state.rgdwPOV[1U] == 0xffffffffU);
    CHECK(extended_state.lVX == 0L);
    CHECK(extended_state.lAX == 0L);
    CHECK(extended_state.lFX == 0L);
    CHECK(extended_state.rgbButtons[127U] == 0U);

    auto opposing_dpad = expected;
    opposing_dpad.buttons = static_cast<button_mask>(gamepad_button::dpad_up) |
                            static_cast<button_mask>(gamepad_button::dpad_down);
    publish_gamepad(owners, 0U, opposing_dpad, 122U);
    REQUIRE(wait_for_sample(*owners.core, target, opposing_dpad));
    REQUIRE(device->GetDeviceState(data_size<DIJOYSTATE2>(), &extended_state) == DI_OK);
    CHECK(extended_state.rgbButtons[11U] == 0x80U);
    CHECK(extended_state.rgbButtons[12U] == 0x80U);
    CHECK(extended_state.rgdwPOV[0U] == 0xffffffffU);

    publish_gamepad(owners, 0U, expected, 123U);
    REQUIRE(wait_for_sample(*owners.core, target, expected));
    REQUIRE(device->Unacquire() == DI_OK);
    REQUIRE(device->SetDataFormat(standard_format) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    auto range = x_range_property();
    REQUIRE(inputnext_dinput_get_property_w(device.get(), &DIPROP_RANGE, &range.diph) == DI_OK);
    CHECK(range.lMin == 0L);
    CHECK(range.lMax == 65535L);
    auto deadzone = x_dword_property();
    REQUIRE(
        inputnext_dinput_get_property_w(device.get(), &DIPROP_DEADZONE, &deadzone.diph) == DI_OK);
    CHECK(deadzone.dwData == 0U);
    auto saturation = x_dword_property();
    REQUIRE(inputnext_dinput_get_property_w(device.get(), &DIPROP_SATURATION, &saturation.diph) ==
            DI_OK);
    CHECK(saturation.dwData == 10000U);
    auto axis_mode = device_dword_property();
    REQUIRE(
        inputnext_dinput_get_property_w(device.get(), &DIPROP_AXISMODE, &axis_mode.diph) == DI_OK);
    CHECK(axis_mode.dwData == DIPROPAXISMODE_ABS);
    auto buffer_size = device_dword_property();
    REQUIRE(inputnext_dinput_get_property_w(device.get(), &DIPROP_BUFFERSIZE, &buffer_size.diph) ==
            DI_OK);
    CHECK(buffer_size.dwData == 0U);
    auto vendor_product = device_dword_property();
    REQUIRE(inputnext_dinput_get_property_w(device.get(), &DIPROP_VIDPID, &vendor_product.diph) ==
            DI_OK);
    CHECK(vendor_product.dwData == 0x054c0ce6U);
    auto gain = device_dword_property(7500U);
    CHECK(inputnext_dinput_get_property_w(device.get(), &DIPROP_FFGAIN, &gain.diph) ==
          DIERR_UNSUPPORTED);
    CHECK(gain.dwData == 7500U);
    CHECK(inputnext_dinput_set_property_w(device.get(), &DIPROP_FFGAIN, &gain.diph) ==
          DIERR_UNSUPPORTED);

    const auto changed_range = x_range_property();
    auto configured_range = changed_range;
    configured_range.lMin = 100L;
    configured_range.lMax = 1100L;
    CHECK(inputnext_dinput_set_property_w(device.get(), &DIPROP_RANGE, &configured_range.diph) ==
          DIERR_ACQUIRED);
    REQUIRE(device->Unacquire() == DI_OK);
    const auto absolute_mode = device_dword_property(DIPROPAXISMODE_ABS);
    CHECK(inputnext_dinput_set_property_w(device.get(), &DIPROP_AXISMODE, &absolute_mode.diph) ==
          DI_OK);
    const auto relative_mode = device_dword_property(DIPROPAXISMODE_REL);
    CHECK(inputnext_dinput_set_property_w(device.get(), &DIPROP_AXISMODE, &relative_mode.diph) ==
          DIERR_UNSUPPORTED);
    const auto empty_buffer = device_dword_property();
    CHECK(inputnext_dinput_set_property_w(device.get(), &DIPROP_BUFFERSIZE, &empty_buffer.diph) ==
          DI_OK);
    const auto requested_buffer = device_dword_property(8U);
    CHECK(
        inputnext_dinput_set_property_w(device.get(), &DIPROP_BUFFERSIZE, &requested_buffer.diph) ==
        DIERR_UNSUPPORTED);
    REQUIRE(inputnext_dinput_set_property_w(device.get(), &DIPROP_RANGE, &configured_range.diph) ==
            DI_OK);
    const auto configured_deadzone = x_dword_property(5000U);
    REQUIRE(inputnext_dinput_set_property_w(device.get(),
                &DIPROP_DEADZONE,
                &configured_deadzone.diph) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);
    REQUIRE(device->GetDeviceState(data_size<DIJOYSTATE>(), &state) == DI_OK);
    CHECK(state.lX == 600L);
    REQUIRE(device->Unacquire() == DI_OK);
    const auto cleared_deadzone = x_dword_property();
    REQUIRE(
        inputnext_dinput_set_property_w(device.get(), &DIPROP_DEADZONE, &cleared_deadzone.diph) ==
        DI_OK);
    REQUIRE(device->Acquire() == DI_OK);
    REQUIRE(device->GetDeviceState(data_size<DIJOYSTATE>(), &state) == DI_OK);
    CHECK(state.lX == 725L);

    CHECK(device->Unacquire() == DI_OK);
    CHECK(device->Unacquire() == DI_OK);
}

TEST_CASE("dinput.no_sample_is_pending_without_changing_the_callers_buffer",
    "[integration][dinput][input][immediate]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U, false);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent, 0x35U);
    const hidden_window window;
    configure_compact_device(*device.get(), window.get());
    REQUIRE(device->Acquire() == DI_OK);

    auto state = sentinel_state(0xD3U);
    const auto sentinel = state;
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == E_PENDING);
    CHECK(state == sentinel);

    const auto expected = sample_state();
    publish_gamepad(owners, 0U, expected);
    REQUIRE(wait_for_sample(*owners.core, target, expected));
    CHECK(device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    CHECK(state.buttons[0U] == 0x80U);
}

TEST_CASE("dinput.device_supports_custom_formats_and_reports_unsupported_operations",
    "[integration][dinput][input][format][unsupported]") {
    core_test_support::fixture owners;
    const auto target = owners.connect(0U);
    const auto expected = sample_state();
    publish_gamepad(owners, 0U, expected);
    REQUIRE(wait_for_sample(*owners.core, target, expected));

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    auto device = create_device(target, access, parent);
    std::array<DIOBJECTDATAFORMAT, 22U> descriptors{};
    std::array<GUID, 22U> guids{};
    const auto format = compact_format(descriptors, guids);
    REQUIRE(device->SetDataFormat(&format) == DI_OK);

    const auto malformed = format;
    descriptors[1U].dwOfs = descriptors[0U].dwOfs;
    CHECK(device->SetDataFormat(&malformed) == DIERR_INVALIDPARAM);
    descriptors[1U].dwOfs = static_cast<DWORD>(offsetof(compact_state, axes) + sizeof(LONG));

    const hidden_window window;
    CHECK(device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND) == DIERR_INVALIDPARAM);
    CHECK(device->SetCooperativeLevel(window.get(),
              DISCL_BACKGROUND | DISCL_NONEXCLUSIVE | DISCL_EXCLUSIVE) == DIERR_INVALIDPARAM);
    CHECK(device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_EXCLUSIVE) ==
          DIERR_UNSUPPORTED);
    REQUIRE(
        device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) == DI_OK);
    REQUIRE(device->Acquire() == DI_OK);

    compact_state state{};
    CHECK(device->GetDeviceState(data_size<compact_state>() - data_size<DWORD>(), &state) ==
          DIERR_INVALIDPARAM);
    CHECK(device->SetDataFormat(&format) == DIERR_ACQUIRED);

    DIDEVICEOBJECTDATA event{};
    DWORD count = 1U;
    CHECK(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), &event, &count, 0U) ==
          DIERR_NOTBUFFERED);
    CHECK(count == 0U);
    CHECK(device->SetEventNotification(nullptr) == DIERR_ACQUIRED);
    auto* const sentinel_event = GetCurrentProcess();
    CHECK(device->SetEventNotification(sentinel_event) == DIERR_ACQUIRED);

    DWORD force_feedback_state{};
    CHECK(device->GetForceFeedbackState(&force_feedback_state) == DIERR_UNSUPPORTED);
    CHECK(device->SendForceFeedbackCommand(DISFFC_RESET) == DIERR_UNSUPPORTED);
    alignas(void*) std::byte effect_storage{};
    auto* effect = reinterpret_cast<IDirectInputEffect*>(&effect_storage);
    const DIEFFECT effect_description{};
    CHECK(device->CreateEffect(GUID_ConstantForce, &effect_description, &effect, nullptr) ==
          DIERR_UNSUPPORTED);
    CHECK(effect == nullptr);

    DWORD effect_callbacks{};
    CHECK(device->EnumEffects(count_effect_callback, &effect_callbacks, DIEFT_ALL) == DI_OK);
    CHECK(effect_callbacks == 0U);

    CHECK(device->Unacquire() == DI_OK);
    CHECK(device->SetEventNotification(nullptr) == DI_OK);
    CHECK(device->SetEventNotification(sentinel_event) == DIERR_UNSUPPORTED);
    CHECK(device->SetDataFormat(&format) == DI_OK);
}

TEST_CASE("dinput.devices_cover_more_than_xinput_positions_and_ignore_xinput_disable",
    "[integration][dinput][input][coexistence]") {
    core_test_support::fixture owners;
    constexpr std::size_t device_count = 5U;
    std::array<connection_target, device_count> targets{};
    for (std::size_t index = 0U; index < device_count; ++index) {
        targets[index] = owners.connect(index);
    }

    const auto expected = sample_state();
    publish_gamepad(owners, 0U, expected);
    REQUIRE(wait_for_sample(*owners.core, targets[0U], expected));
    const auto latest = owners.core->read_latest(targets[0U]);
    REQUIRE(latest.has_value());
    CHECK(latest->sample.state == expected);

    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    std::array<com_owner<IDirectInputDevice8W>, device_count> devices{};
    for (std::size_t index = 0U; index < device_count; ++index) {
        devices[index] =
            create_device(targets[index], access, parent, static_cast<std::uint8_t>(0x40U + index));
        configure_compact_device(*devices[index].get(), window.get());
        REQUIRE(devices[index]->Acquire() == DI_OK);
    }

    owners.assign_xinput(targets[0U]);
    XINPUT_STATE xinput_state{};
    CHECK(owners.presentation->get_state(xinput::flavor::v1_4, 0U, &xinput_state) == ERROR_SUCCESS);
    CHECK((xinput_state.Gamepad.wButtons & 0x3000U) == 0x3000U);

    const auto current_calls = owners.gameinput.native->current_calls;
    const auto facts_calls = owners.gameinput.native->facts_calls;
    const auto history_factories = owners.scripts[0U]->cursor_factories.load();
    const auto rumble = owners.gameinput.native->rumble_script();
    const auto output_factories = rumble->factory_calls();
    const auto output_writes = rumble->writes();
    for (std::size_t index = 0U; index < device_count; ++index) {
        compact_state state{};
        REQUIRE(devices[index]->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
        if (index == 0U) {
            CHECK(state.buttons[0U] == 0x80U);
            CHECK(state.buttons[1U] == 0x80U);
            CHECK(state.pov == 4500U);
        }
    }

    CHECK(owners.gameinput.native->current_calls == current_calls);
    CHECK(owners.gameinput.native->facts_calls == facts_calls);
    CHECK(owners.scripts[0U]->cursor_factories.load() == history_factories);
    CHECK(rumble->factory_calls() == output_factories);
    CHECK(rumble->writes() == output_writes);

    const auto packet_before_disable = xinput_state.dwPacketNumber;
    owners.presentation->enable(FALSE);
    CHECK(owners.presentation->get_state(xinput::flavor::v1_4, 0U, &xinput_state) == ERROR_SUCCESS);
    CHECK(xinput_state.dwPacketNumber == packet_before_disable);
    CHECK(xinput_state.Gamepad.wButtons == 0U);
    CHECK(xinput_state.Gamepad.bLeftTrigger == 0U);
    CHECK(xinput_state.Gamepad.bRightTrigger == 0U);
    CHECK(xinput_state.Gamepad.sThumbLX == 0);
    CHECK(xinput_state.Gamepad.sThumbLY == 0);
    CHECK(xinput_state.Gamepad.sThumbRX == 0);
    CHECK(xinput_state.Gamepad.sThumbRY == 0);
    compact_state still_available{};
    CHECK(devices[0U]->GetDeviceState(data_size<compact_state>(), &still_available) == DI_OK);
    CHECK(still_available.buttons[0U] == 0x80U);
    CHECK(still_available.buttons[1U] == 0x80U);
    CHECK(still_available.axes[0U] == 40959L);
    CHECK(still_available.axes[1U] == 40959L);
    CHECK(still_available.axes[2U] == 16384L);
    CHECK(still_available.axes[3U] == 24575L);
    CHECK(still_available.axes[4U] == 24575L);
    CHECK(still_available.axes[5U] == 49151L);
    CHECK(still_available.pov == 4500U);
    const auto catalogue = owners.core->enumerate();
    REQUIRE(catalogue.has_value());
    CHECK(catalogue->count == device_count);
}

TEST_CASE("dinput.old_objects_remain_bound_to_the_retired_connection_after_reconnect",
    "[integration][dinput][input][reconnect]") {
    core_test_support::fixture owners;
    const auto old_target = owners.connect(0U);
    parent_unknown parent;
    const auto access = make_access(owners, *owners.focus_observer);
    const hidden_window window;
    auto old_device = create_device(old_target, access, parent, 0x61U);
    configure_compact_device(*old_device.get(), window.get());
    REQUIRE(old_device->Acquire() == DI_OK);
    DIDEVCAPS old_capabilities{};
    old_capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(old_device->GetCapabilities(&old_capabilities) == DI_OK);
    CHECK((old_capabilities.dwFlags & DIDC_ATTACHED) != 0U);
    compact_state state{};
    REQUIRE(old_device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);

    owners.gameinput.native->emit(
        gameinput_test_support::handle(owners.gameinput.device_memory[0U]),
        gameinput_detail::native_device_event::disconnected);
    for (std::size_t attempt = 0U;
        attempt < 32U && owners.gameinput.runtime->connections().size != 0U;
        ++attempt) {
        owners.gameinput.maintain();
    }
    REQUIRE(owners.gameinput.runtime->connections().size == 0U);
    old_capabilities = {};
    old_capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(old_device->GetCapabilities(&old_capabilities) == DI_OK);
    CHECK((old_capabilities.dwFlags & DIDC_ATTACHED) == 0U);

    const auto new_target = owners.connect(0U);
    REQUIRE(new_target != old_target);
    CHECK(old_device->GetDeviceState(data_size<compact_state>(), &state) == DIERR_INPUTLOST);
    CHECK(old_device->Acquire() == DIERR_INPUTLOST);
    old_capabilities = {};
    old_capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(old_device->GetCapabilities(&old_capabilities) == DI_OK);
    CHECK((old_capabilities.dwFlags & DIDC_ATTACHED) == 0U);

    auto new_device = create_device(new_target, access, parent, 0x62U);
    configure_compact_device(*new_device.get(), window.get());
    REQUIRE(new_device->Acquire() == DI_OK);
    CHECK(new_device->GetDeviceState(data_size<compact_state>(), &state) == DI_OK);
    DIDEVCAPS new_capabilities{};
    new_capabilities.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(new_device->GetCapabilities(&new_capabilities) == DI_OK);
    CHECK((new_capabilities.dwFlags & DIDC_ATTACHED) != 0U);
}
