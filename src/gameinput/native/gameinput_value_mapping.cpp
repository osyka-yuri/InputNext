#include "gameinput_value_mapping.hpp"

#include <cstring>

#include <GameInput.h>
#include <inputnext/device_metadata.hpp>
#include <inputnext/gamepad_state.hpp>

namespace inputnext::gameinput_detail {
namespace {

namespace gameinput = GameInput::v4;

void copy_button(gameinput::GameInputGamepadButtons native_buttons,
    gameinput::GameInputGamepadButtons source,
    gamepad_button destination,
    button_mask& buttons) noexcept {
    if ((native_buttons & source) != 0) {
        buttons |= static_cast<button_mask>(destination);
    }
}

} // namespace

haptic_info_result classify_haptic_info_result(HRESULT result) noexcept {
    if (result == gameinput::GAMEINPUT_E_HAPTIC_INFO_NOT_FOUND) {
        return haptic_info_result::not_present;
    }
    return FAILED(result) ? haptic_info_result::failure : haptic_info_result::available;
}

gamepad_state map_gamepad_state(const gameinput::GameInputGamepadState& native_state) noexcept {
    button_mask buttons{};
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadDPadUp,
        gamepad_button::dpad_up,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadDPadDown,
        gamepad_button::dpad_down,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadDPadLeft,
        gamepad_button::dpad_left,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadDPadRight,
        gamepad_button::dpad_right,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadMenu,
        gamepad_button::menu,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadView,
        gamepad_button::view,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadLeftThumbstick,
        gamepad_button::left_stick,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadRightThumbstick,
        gamepad_button::right_stick,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadLeftShoulder,
        gamepad_button::left_shoulder,
        buttons);
    copy_button(native_state.buttons,
        gameinput::GameInputGamepadRightShoulder,
        gamepad_button::right_shoulder,
        buttons);
    copy_button(native_state.buttons, gameinput::GameInputGamepadA, gamepad_button::south, buttons);
    copy_button(native_state.buttons, gameinput::GameInputGamepadB, gamepad_button::east, buttons);
    copy_button(native_state.buttons, gameinput::GameInputGamepadX, gamepad_button::west, buttons);
    copy_button(native_state.buttons, gameinput::GameInputGamepadY, gamepad_button::north, buttons);

    return gamepad_state{
        .buttons = buttons,
        .left_stick =
            stick_state{.x = native_state.leftThumbstickX, .y = native_state.leftThumbstickY},
        .right_stick =
            stick_state{.x = native_state.rightThumbstickX, .y = native_state.rightThumbstickY},
        .left_trigger = native_state.leftTrigger,
        .right_trigger = native_state.rightTrigger,
    };
}

opaque_device_id copy_device_id(const APP_LOCAL_DEVICE_ID& native_id) noexcept {
    static_assert(sizeof(APP_LOCAL_DEVICE_ID) == gameinput_device_id_bytes);
    opaque_device_id result{};
    std::memcpy(result.bytes.data(), &native_id, result.bytes.size());
    return result;
}

native_guid copy_guid(const GUID& value) noexcept {
    native_guid result{};
    std::memcpy(result.bytes.data(), &value, result.bytes.size());
    return result;
}

} // namespace inputnext::gameinput_detail
