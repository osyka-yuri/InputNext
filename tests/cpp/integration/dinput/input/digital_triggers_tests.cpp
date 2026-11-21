#include "cpp/integration/core/core_test_support.hpp"
#include "cpp/integration/dinput/input/buffered_test_support.hpp"
#include "cpp/integration/dinput/input/support.hpp"
#include "dinput/input_access.hpp"
#include "dinput/profile.hpp"
#include "dinput/system_input.hpp"
#include "gameinput/native/native_history.hpp"

#include <catch2/catch_test_macros.hpp>

#include <Windows.h>
#include <dinput.h>
#include <inputnext/device_identity.hpp>
#include <inputnext/gamepad_state.hpp>
#include <inputnext/input_sample.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace inputnext;
using namespace inputnext::dinput_test_support;

namespace {

struct object_collector final {
    std::vector<DIDEVICEOBJECTINSTANCEW> objects;
};

BOOL CALLBACK collect_objects_callback(const DIDEVICEOBJECTINSTANCEW* const object,
    void* const context) {
    if (object != nullptr && context != nullptr) {
        static_cast<object_collector*>(context)->objects.push_back(*object);
    }
    return DIENUM_CONTINUE;
}

[[nodiscard]] gamepad_state idle_state() noexcept {
    return gamepad_state{};
}

struct trigger_device_fixture final {
    core_test_support::fixture owners;
    connection_target target;
    parent_unknown parent;
    const hidden_window window;
    std::shared_ptr<dinput::input_access> access;
    com_owner<IDirectInputDevice8W> device;

    explicit trigger_device_fixture(dinput::device_model model = dinput::device_model{true, 0.5},
        std::uint8_t instance_byte = 0x31U)
        : target(owners.connect(0U)),
          access(make_access(owners, *owners.focus_observer, model)),
          device(create_device(target, access, parent, instance_byte)) {}

    void acquire(const DIDATAFORMAT* format = dinput::system_joystick_format()) {
        REQUIRE(format != nullptr);
        REQUIRE(device->SetDataFormat(format) == DI_OK);
        REQUIRE(device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) ==
                DI_OK);
        REQUIRE(device->Acquire() == DI_OK);
    }

    void prepare_buffered(DWORD buffer_size = 32U) {
        prepare_buffered_history(owners);
        const auto* format = dinput::system_joystick_format();
        REQUIRE(format != nullptr);
        REQUIRE(device->SetDataFormat(format) == DI_OK);
        REQUIRE(device->SetCooperativeLevel(window.get(), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) ==
                DI_OK);
        REQUIRE(set_buffer_size(*device.get(), buffer_size) == DI_OK);
        REQUIRE(device->Acquire() == DI_OK);
    }

    void publish(const gamepad_state& state) {
        publish_gamepad(owners, 0U, state);
        REQUIRE(wait_for_sample(*owners.core, target, state));
    }

    void enqueue_reading(std::uint64_t timestamp, const gamepad_state& state) {
        const auto script = owners.scripts[0U];
        script->enqueue(gameinput_detail::native_history_step{
            .kind = gameinput_detail::native_history_step_kind::reading,
            .reading =
                input_update{
                    .state = state,
                    .source_timestamp_us = timestamp,
                    .observation_qpc =
                        script->seed.baseline.observation_qpc + timestamp - buffered_cut_timestamp,
                },
        });
    }

    void synchronize_buffered_input() {
        enqueue_reading(buffered_cut_timestamp + 1U, gamepad_state{});
        enqueue_checkpoint(owners.scripts[0U], buffered_cut_timestamp + 1U);

        REQUIRE(wait_for_buffer_count(*device.get(), 1U));
        std::array<DIDEVICEOBJECTDATA, 32> initial_events{};
        auto initial_count = static_cast<DWORD>(initial_events.size());
        REQUIRE(device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(),
                    initial_events.data(),
                    &initial_count,
                    0U) == DI_OK);
    }
};

} // namespace

TEST_CASE("dinput.digital_triggers.standard_model_exact_backward_compatibility",
    "[integration][dinput][input][digital_triggers]") {
    trigger_device_fixture fix{dinput::device_model{false}};

    DIDEVCAPS caps{};
    caps.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(fix.device->GetCapabilities(&caps) == DI_OK);
    CHECK(caps.dwButtons == 15U);
    CHECK(caps.dwAxes == 6U);
    CHECK(caps.dwPOVs == 1U);

    object_collector collector;
    REQUIRE(fix.device->EnumObjects(&collect_objects_callback, &collector, DIDFT_ALL) == DI_OK);
    REQUIRE(collector.objects.size() == 22U);

    for (const auto& obj : collector.objects) {
        if (DIDFT_GETTYPE(obj.dwType) == DIDFT_PSHBUTTON) {
            CHECK(DIDFT_GETINSTANCE(obj.dwType) < 15U);
        }
    }

    DIDEVICEOBJECTINSTANCEW queried{};
    queried.dwSize = data_size<DIDEVICEOBJECTINSTANCEW>();
    const DWORD button15_id = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(15U);
    CHECK(fix.device->GetObjectInfo(&queried, button15_id, DIPH_BYID) == DIERR_OBJECTNOTFOUND);
    const DWORD button16_id = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(16U);
    CHECK(fix.device->GetObjectInfo(&queried, button16_id, DIPH_BYID) == DIERR_OBJECTNOTFOUND);

    fix.acquire();

    gamepad_state state{};
    state.left_trigger = 1.0F;
    state.right_trigger = 1.0F;
    fix.publish(state);

    DIJOYSTATE joy{};
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.lZ == 65535L);
    CHECK(joy.lRz == 65535L);
    CHECK(joy.rgbButtons[15] == 0U);
    CHECK(joy.rgbButtons[16] == 0U);
}

TEST_CASE("dinput.digital_triggers.extended_model_enumerates_17_buttons_in_specified_order",
    "[integration][dinput][input][digital_triggers]") {
    trigger_device_fixture fix;

    DIDEVCAPS caps{};
    caps.dwSize = data_size<DIDEVCAPS>();
    REQUIRE(fix.device->GetCapabilities(&caps) == DI_OK);
    CHECK(caps.dwButtons == 17U);
    CHECK(caps.dwAxes == 6U);
    CHECK(caps.dwPOVs == 1U);

    object_collector collector;
    REQUIRE(fix.device->EnumObjects(&collect_objects_callback, &collector, DIDFT_ALL) == DI_OK);
    REQUIRE(collector.objects.size() == 24U);

    for (std::size_t i = 0; i < 6; ++i) {
        CHECK(DIDFT_GETTYPE(collector.objects[i].dwType) == DIDFT_ABSAXIS);
    }
    for (std::size_t i = 6; i <= 20; ++i) {
        CHECK(DIDFT_GETTYPE(collector.objects[i].dwType) == DIDFT_PSHBUTTON);
        CHECK(DIDFT_GETINSTANCE(collector.objects[i].dwType) == static_cast<DWORD>(i - 6));
    }
    CHECK(DIDFT_GETTYPE(collector.objects[21].dwType) == DIDFT_POV);
    CHECK(DIDFT_GETINSTANCE(collector.objects[21].dwType) == 0U);

    const auto& lt_obj = collector.objects[22];
    CHECK(DIDFT_GETTYPE(lt_obj.dwType) == DIDFT_PSHBUTTON);
    CHECK(DIDFT_GETINSTANCE(lt_obj.dwType) == 15U);
    CHECK(lt_obj.guidType == GUID_Button);
    CHECK(lt_obj.dwOfs == DIJOFS_BUTTON(15));
    CHECK(lt_obj.wUsagePage == 0x09U);
    CHECK(lt_obj.wUsage == 16U);
    CHECK(std::wstring(lt_obj.tszName) == L"LT");

    const auto& rt_obj = collector.objects[23];
    CHECK(DIDFT_GETTYPE(rt_obj.dwType) == DIDFT_PSHBUTTON);
    CHECK(DIDFT_GETINSTANCE(rt_obj.dwType) == 16U);
    CHECK(rt_obj.guidType == GUID_Button);
    CHECK(rt_obj.dwOfs == DIJOFS_BUTTON(16));
    CHECK(rt_obj.wUsagePage == 0x09U);
    CHECK(rt_obj.wUsage == 17U);
    CHECK(std::wstring(rt_obj.tszName) == L"RT");

    object_collector button_collector;
    REQUIRE(fix.device->EnumObjects(&collect_objects_callback, &button_collector, DIDFT_BUTTON) ==
            DI_OK);
    CHECK(button_collector.objects.size() == 17U);
}

TEST_CASE("dinput.digital_triggers.get_object_info_by_id_offset_and_usage",
    "[integration][dinput][input][digital_triggers]") {
    trigger_device_fixture fix;
    REQUIRE(fix.device->SetDataFormat(dinput::system_joystick_format()) == DI_OK);

    DIDEVICEOBJECTINSTANCEW queried{};
    queried.dwSize = data_size<DIDEVICEOBJECTINSTANCEW>();

    const DWORD lt_id = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(15U);
    REQUIRE(fix.device->GetObjectInfo(&queried, lt_id, DIPH_BYID) == DI_OK);
    CHECK(std::wstring(queried.tszName) == L"LT");
    CHECK(queried.dwOfs == DIJOFS_BUTTON(15));

    const DWORD rt_id = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(16U);
    REQUIRE(fix.device->GetObjectInfo(&queried, rt_id, DIPH_BYID) == DI_OK);
    CHECK(std::wstring(queried.tszName) == L"RT");
    CHECK(queried.dwOfs == DIJOFS_BUTTON(16));

    const DWORD nonexistent_id = DIDFT_BUTTON | DIDFT_MAKEINSTANCE(17U);
    CHECK(fix.device->GetObjectInfo(&queried, nonexistent_id, DIPH_BYID) == DIERR_OBJECTNOTFOUND);

    REQUIRE(fix.device->GetObjectInfo(&queried, DIJOFS_BUTTON(15), DIPH_BYOFFSET) == DI_OK);
    CHECK(std::wstring(queried.tszName) == L"LT");

    REQUIRE(fix.device->GetObjectInfo(&queried, DIJOFS_BUTTON(16), DIPH_BYOFFSET) == DI_OK);
    CHECK(std::wstring(queried.tszName) == L"RT");

    const auto lt_usage = DIMAKEUSAGEDWORD(0x09U, 16U);
    REQUIRE(fix.device->GetObjectInfo(&queried, lt_usage, DIPH_BYUSAGE) == DI_OK);
    CHECK(std::wstring(queried.tszName) == L"LT");

    const auto rt_usage = DIMAKEUSAGEDWORD(0x09U, 17U);
    REQUIRE(fix.device->GetObjectInfo(&queried, rt_usage, DIPH_BYUSAGE) == DI_OK);
    CHECK(std::wstring(queried.tszName) == L"RT");
}

TEST_CASE("dinput.digital_triggers.immediate_state_threshold_boundaries_and_analog_preservation",
    "[integration][dinput][input][digital_triggers]") {
    trigger_device_fixture fix;
    fix.acquire();

    gamepad_state state{};
    state.left_trigger = 0.499F;
    state.right_trigger = 0.499F;
    fix.publish(state);

    DIJOYSTATE joy{};
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x00U);
    CHECK(joy.rgbButtons[16] == 0x00U);
    CHECK(joy.lZ > 32000L);
    CHECK(joy.lRz > 32000L);

    state.left_trigger = 0.500F;
    state.right_trigger = 0.499F;
    fix.publish(state);

    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x80U);
    CHECK(joy.rgbButtons[16] == 0x00U);
    CHECK(joy.lZ == 32768L);

    state.left_trigger = 0.75F;
    state.right_trigger = 0.75F;
    fix.publish(state);

    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x80U);
    CHECK(joy.rgbButtons[16] == 0x80U);
    CHECK(joy.lZ == 49151L);
    CHECK(joy.lRz == 49151L);

    state.left_trigger = 0.0F;
    state.right_trigger = 0.0F;
    fix.publish(state);

    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x00U);
    CHECK(joy.rgbButtons[16] == 0x00U);
    CHECK(joy.lZ == 0L);
    CHECK(joy.lRz == 0L);

    REQUIRE(fix.device->Unacquire() == DI_OK);
    REQUIRE(fix.device->SetDataFormat(&c_dfDIJoystick2) == DI_OK);
    REQUIRE(fix.device->Acquire() == DI_OK);

    state.left_trigger = 0.8F;
    state.right_trigger = 0.2F;
    fix.publish(state);

    DIJOYSTATE2 joy2{};
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE2>(), &joy2) == DI_OK);
    CHECK(joy2.rgbButtons[15] == 0x80U);
    CHECK(joy2.rgbButtons[16] == 0x00U);
    CHECK(joy2.lZ > 50000L);
    CHECK(joy2.lRz < 20000L);
}

TEST_CASE("dinput.digital_triggers.independence_of_triggers_and_bumpers",
    "[integration][dinput][input][digital_triggers]") {
    trigger_device_fixture fix;
    fix.acquire();

    DIJOYSTATE joy{};

    gamepad_state state{};
    state.buttons = static_cast<button_mask>(gamepad_button::left_shoulder);
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[4] == 0x80U);
    CHECK(joy.rgbButtons[15] == 0x00U);

    state = idle_state();
    state.left_trigger = 0.9F;
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[4] == 0x00U);
    CHECK(joy.rgbButtons[15] == 0x80U);

    state.buttons = static_cast<button_mask>(gamepad_button::left_shoulder);
    state.left_trigger = 0.9F;
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[4] == 0x80U);
    CHECK(joy.rgbButtons[15] == 0x80U);

    state = idle_state();
    state.buttons = static_cast<button_mask>(gamepad_button::right_shoulder);
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[5] == 0x80U);
    CHECK(joy.rgbButtons[16] == 0x00U);

    state = idle_state();
    state.right_trigger = 0.9F;
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[5] == 0x00U);
    CHECK(joy.rgbButtons[16] == 0x80U);

    state.buttons = static_cast<button_mask>(gamepad_button::right_shoulder);
    state.right_trigger = 0.9F;
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[5] == 0x80U);
    CHECK(joy.rgbButtons[16] == 0x80U);

    state.buttons = static_cast<button_mask>(gamepad_button::left_shoulder) |
                    static_cast<button_mask>(gamepad_button::right_shoulder);
    state.left_trigger = 1.0F;
    state.right_trigger = 1.0F;
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[4] == 0x80U);
    CHECK(joy.rgbButtons[5] == 0x80U);
    CHECK(joy.rgbButtons[15] == 0x80U);
    CHECK(joy.rgbButtons[16] == 0x80U);
    CHECK(joy.lZ == 65535L);
    CHECK(joy.lRz == 65535L);
}

TEST_CASE("dinput.digital_triggers.custom_threshold_configuration",
    "[integration][dinput][input][digital_triggers]") {
    {
        trigger_device_fixture fix{dinput::device_model{true, 0.8}};
        fix.acquire();

        gamepad_state state{};
        state.left_trigger = 0.79F;
        fix.publish(state);
        DIJOYSTATE joy{};
        REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
        CHECK(joy.rgbButtons[15] == 0x00U);

        state.left_trigger = 0.80F;
        fix.publish(state);
        REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
        CHECK(joy.rgbButtons[15] == 0x80U);
    }

    {
        trigger_device_fixture fix{dinput::device_model{true, 0.15}, 0x32U};
        fix.acquire();

        gamepad_state state{};
        state.right_trigger = 0.14F;
        fix.publish(state);
        DIJOYSTATE joy{};
        REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
        CHECK(joy.rgbButtons[16] == 0x00U);

        state.right_trigger = 0.15F;
        fix.publish(state);
        REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
        CHECK(joy.rgbButtons[16] == 0x80U);
    }
}

TEST_CASE("dinput.digital_triggers.buffered_input_events_press_and_release",
    "[integration][dinput][input][digital_triggers][buffered]") {
    trigger_device_fixture fix{dinput::device_model{true, 0.5}, 0x71U};
    fix.prepare_buffered(32U);

    const auto script = fix.owners.scripts[0U];

    fix.synchronize_buffered_input();

    gamepad_state state2{};
    state2.left_trigger = 0.7F;
    fix.enqueue_reading(buffered_cut_timestamp + 2U, state2);

    gamepad_state state3{};
    state3.left_trigger = 0.2F;
    fix.enqueue_reading(buffered_cut_timestamp + 3U, state3);

    gamepad_state state4{};
    state4.right_trigger = 0.9F;
    fix.enqueue_reading(buffered_cut_timestamp + 4U, state4);

    gamepad_state state5{};
    state5.right_trigger = 0.1F;
    fix.enqueue_reading(buffered_cut_timestamp + 5U, state5);

    enqueue_checkpoint(script, buffered_cut_timestamp + 6U);

    REQUIRE(wait_for_buffer_count(*fix.device.get(), 9U));

    std::array<DIDEVICEOBJECTDATA, 32> events{};
    auto count = static_cast<DWORD>(events.size());
    REQUIRE(fix.device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) ==
            DI_OK);
    REQUIRE(count == 9U);

    const std::array expected_events{
        std::pair<DWORD, DWORD>{DIJOFS_Z, 45874U},
        std::pair<DWORD, DWORD>{DIJOFS_BUTTON(15), 0x80U},
        std::pair<DWORD, DWORD>{DIJOFS_Z, 13107U},
        std::pair<DWORD, DWORD>{DIJOFS_BUTTON(15), 0x00U},
        std::pair<DWORD, DWORD>{DIJOFS_Z, 0U},
        std::pair<DWORD, DWORD>{DIJOFS_RZ, 58981U},
        std::pair<DWORD, DWORD>{DIJOFS_BUTTON(16), 0x80U},
        std::pair<DWORD, DWORD>{DIJOFS_RZ, 6554U},
        std::pair<DWORD, DWORD>{DIJOFS_BUTTON(16), 0x00U},
    };
    for (std::size_t index = 0U; index < expected_events.size(); ++index) {
        CHECK(events[index].dwOfs == expected_events[index].first);
        CHECK(events[index].dwData == expected_events[index].second);
    }
}

TEST_CASE("dinput.digital_triggers.disabled_buffered_input_does_not_emit_digital_trigger_events",
    "[integration][dinput][input][digital_triggers][buffered]") {
    trigger_device_fixture fix{dinput::device_model{false}, 0x72U};
    fix.prepare_buffered(32U);

    const auto script = fix.owners.scripts[0U];

    fix.synchronize_buffered_input();

    gamepad_state state2{};
    state2.left_trigger = 0.8F;
    state2.right_trigger = 0.8F;
    fix.enqueue_reading(buffered_cut_timestamp + 2U, state2);

    gamepad_state state3{};
    state3.left_trigger = 0.1F;
    state3.right_trigger = 0.1F;
    fix.enqueue_reading(buffered_cut_timestamp + 3U, state3);

    enqueue_checkpoint(script, buffered_cut_timestamp + 4U);

    REQUIRE(wait_for_buffer_count(*fix.device.get(), 4U));

    std::array<DIDEVICEOBJECTDATA, 32> events{};
    auto count = static_cast<DWORD>(events.size());
    REQUIRE(fix.device->GetDeviceData(data_size<DIDEVICEOBJECTDATA>(), events.data(), &count, 0U) ==
            DI_OK);
    REQUIRE(count >= 4U);

    const std::array expected_events{
        std::pair<DWORD, DWORD>{DIJOFS_Z, 52428U},
        std::pair<DWORD, DWORD>{DIJOFS_RZ, 52428U},
        std::pair<DWORD, DWORD>{DIJOFS_Z, 6554U},
        std::pair<DWORD, DWORD>{DIJOFS_RZ, 6554U},
    };
    REQUIRE(count == static_cast<DWORD>(expected_events.size()));
    for (std::size_t index = 0U; index < expected_events.size(); ++index) {
        CHECK(events[index].dwOfs == expected_events[index].first);
        CHECK(events[index].dwData == expected_events[index].second);
        CHECK((events[index].dwOfs == DIJOFS_Z || events[index].dwOfs == DIJOFS_RZ));
    }
}

TEST_CASE("dinput.digital_triggers.reacquire_consistency",
    "[integration][dinput][input][digital_triggers]") {
    trigger_device_fixture fix;
    const auto* format = dinput::system_joystick_format();
    REQUIRE(fix.device->SetDataFormat(format) == DI_OK);
    REQUIRE(fix.device->SetCooperativeLevel(fix.window.get(),
                DISCL_BACKGROUND | DISCL_NONEXCLUSIVE) == DI_OK);

    REQUIRE(fix.device->Acquire() == DI_OK);
    gamepad_state state{};
    state.left_trigger = 0.8F;
    fix.publish(state);

    DIJOYSTATE joy{};
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x80U);

    REQUIRE(fix.device->Unacquire() == DI_OK);
    CHECK(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DIERR_NOTACQUIRED);

    // Reacquire with the trigger still held.
    REQUIRE(fix.device->Acquire() == DI_OK);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x80U);

    state.left_trigger = 0.1F;
    fix.publish(state);
    REQUIRE(fix.device->GetDeviceState(data_size<DIJOYSTATE>(), &joy) == DI_OK);
    CHECK(joy.rgbButtons[15] == 0x00U);

    REQUIRE(fix.device->Unacquire() == DI_OK);
}
