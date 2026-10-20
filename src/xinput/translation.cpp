#include "translation.hpp"

#include <inputnext/gamepad_state.hpp>

namespace inputnext::xinput::detail {

SHORT to_xinput_axis(float value) noexcept {
    if (value <= -1.0F) {
        return -32768;
    }
    if (value >= 1.0F) {
        return 32767;
    }
    return static_cast<SHORT>(value < 0.0F ? value * 32768.0F : value * 32767.0F);
}

BYTE to_xinput_trigger(float value) noexcept {
    if (value <= 0.0F) {
        return 0U;
    }
    if (value >= 1.0F) {
        return 255U;
    }
    return static_cast<BYTE>(value * 255.0F);
}

WORD to_xinput_buttons(button_mask buttons, bool include_guide) noexcept {
    auto translated = static_cast<WORD>((buttons & 0x03FFU) | ((buttons & 0x3C00U) << 2U));
    if (include_guide && (buttons & static_cast<button_mask>(gamepad_button::guide)) != 0U) {
        translated = static_cast<WORD>(translated | 0x0400U);
    }
    return translated;
}

void translate_state(const gamepad_state& source,
    XINPUT_GAMEPAD& destination,
    bool include_guide) noexcept {
    destination.wButtons = to_xinput_buttons(source.buttons, include_guide);
    destination.bLeftTrigger = to_xinput_trigger(source.left_trigger);
    destination.bRightTrigger = to_xinput_trigger(source.right_trigger);
    destination.sThumbLX = to_xinput_axis(source.left_stick.x);
    destination.sThumbLY = to_xinput_axis(source.left_stick.y);
    destination.sThumbRX = to_xinput_axis(source.right_stick.x);
    destination.sThumbRY = to_xinput_axis(source.right_stick.y);
}

bool same_gamepad(const XINPUT_GAMEPAD& left, const XINPUT_GAMEPAD& right) noexcept {
    return left.wButtons == right.wButtons && left.bLeftTrigger == right.bLeftTrigger &&
           left.bRightTrigger == right.bRightTrigger && left.sThumbLX == right.sThumbLX &&
           left.sThumbLY == right.sThumbLY && left.sThumbRX == right.sThumbRX &&
           left.sThumbRY == right.sThumbRY;
}

} // namespace inputnext::xinput::detail
