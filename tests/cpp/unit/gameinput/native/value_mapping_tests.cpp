#include <inputnext/gamepad_state.hpp>

#include <GameInput.h>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstring>
#include <utility>

#include <gameinput/native/gameinput_value_mapping.hpp>

namespace {

using namespace inputnext;
using namespace inputnext::gameinput_detail;
namespace gameinput = GameInput::v4;

TEST_CASE("gameinput.mapping.haptic_info_result", "[gameinput][native][unit]") {
    CHECK(classify_haptic_info_result(S_OK) == haptic_info_result::available);
    CHECK(classify_haptic_info_result(S_FALSE) == haptic_info_result::available);
    CHECK(classify_haptic_info_result(gameinput::GAMEINPUT_E_HAPTIC_INFO_NOT_FOUND) ==
          haptic_info_result::not_present);
    CHECK(classify_haptic_info_result(E_FAIL) == haptic_info_result::failure);
}

TEST_CASE("gameinput.mapping.buttons", "[gameinput][native][unit]") {
    constexpr std::array mappings{
        std::pair{gameinput::GameInputGamepadDPadUp, gamepad_button::dpad_up},
        std::pair{gameinput::GameInputGamepadDPadDown, gamepad_button::dpad_down},
        std::pair{gameinput::GameInputGamepadDPadLeft, gamepad_button::dpad_left},
        std::pair{gameinput::GameInputGamepadDPadRight, gamepad_button::dpad_right},
        std::pair{gameinput::GameInputGamepadMenu, gamepad_button::menu},
        std::pair{gameinput::GameInputGamepadView, gamepad_button::view},
        std::pair{gameinput::GameInputGamepadLeftThumbstick, gamepad_button::left_stick},
        std::pair{gameinput::GameInputGamepadRightThumbstick, gamepad_button::right_stick},
        std::pair{gameinput::GameInputGamepadLeftShoulder, gamepad_button::left_shoulder},
        std::pair{gameinput::GameInputGamepadRightShoulder, gamepad_button::right_shoulder},
        std::pair{gameinput::GameInputGamepadA, gamepad_button::south},
        std::pair{gameinput::GameInputGamepadB, gamepad_button::east},
        std::pair{gameinput::GameInputGamepadX, gamepad_button::west},
        std::pair{gameinput::GameInputGamepadY, gamepad_button::north},
    };

    for (const auto& [native_button, expected_button] : mappings) {
        gameinput::GameInputGamepadState native_state{};
        native_state.buttons = native_button;
        const auto mapped = map_gamepad_state(native_state);
        CHECK(mapped.buttons == static_cast<button_mask>(expected_button));
    }

    gameinput::GameInputGamepadState unsupported{};
    unsupported.buttons = gameinput::GameInputGamepadC;
    CHECK(map_gamepad_state(unsupported).buttons == 0U);
}

TEST_CASE("gameinput.mapping.analog_and_opaque_identifiers", "[gameinput][native][unit]") {
    gameinput::GameInputGamepadState native_state{};
    native_state.leftTrigger = 0.25F;
    native_state.rightTrigger = 0.75F;
    native_state.leftThumbstickX = -0.5F;
    native_state.leftThumbstickY = 0.5F;
    native_state.rightThumbstickX = -1.0F;
    native_state.rightThumbstickY = 1.0F;
    const auto mapped = map_gamepad_state(native_state);
    CHECK(mapped.left_trigger == 0.25F);
    CHECK(mapped.right_trigger == 0.75F);
    CHECK((mapped.left_stick == stick_state{.x = -0.5F, .y = 0.5F}));
    CHECK((mapped.right_stick == stick_state{.x = -1.0F, .y = 1.0F}));

    APP_LOCAL_DEVICE_ID native_id{};
    std::array<std::byte, sizeof(native_id)> expected_id_bytes{};
    for (std::size_t index = 0U; index < expected_id_bytes.size(); ++index) {
        expected_id_bytes[index] = static_cast<std::byte>(index + 1U);
    }
    std::memcpy(&native_id, expected_id_bytes.data(), expected_id_bytes.size());
    const auto device_id = copy_device_id(native_id);
    CHECK((device_id.bytes == expected_id_bytes));

    GUID native_guid{};
    native_guid.Data1 = 0x12345678U;
    native_guid.Data2 = 0x90ABU;
    native_guid.Data3 = 0xCDEFU;
    native_guid.Data4[0] = 0x42U;
    const auto copied_guid = copy_guid(native_guid);
    CHECK(std::memcmp(copied_guid.bytes.data(), &native_guid, sizeof(native_guid)) == 0);
}

} // namespace
