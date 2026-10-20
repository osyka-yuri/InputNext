#include "xinput/keystroke_mapping.hpp"

#include <array>
#include <cstdint>

namespace inputnext::xinput::detail {
namespace {

constexpr double tan_22_5 = 0.41421356237309504880;
constexpr double tan_67_5 = 2.41421356237309504880;

[[nodiscard]] constexpr key_mask key_bit(std::uint32_t index) noexcept {
    return key_mask{1U} << index;
}

struct stick_position final {
    SHORT x{};
    SHORT y{};
};

struct stick_mapping final {
    SHORT deadzone{};
    std::uint32_t first_key_index{};
};

[[nodiscard]] key_mask map_thumbstick(stick_position position, stick_mapping mapping) noexcept {
    const auto x = static_cast<std::int64_t>(position.x);
    const auto y = static_cast<std::int64_t>(position.y);
    const auto magnitude_squared = (x * x) + (y * y);
    const auto deadzone_squared = static_cast<std::int64_t>(mapping.deadzone) * mapping.deadzone;
    if (magnitude_squared <= deadzone_squared) {
        return 0U;
    }

    const auto absolute_x = static_cast<double>(x < 0 ? -x : x);
    const auto absolute_y = static_cast<double>(y < 0 ? -y : y);
    const bool points_up = y >= 0;
    const bool points_right = x >= 0;

    if (absolute_y < tan_22_5 * absolute_x) {
        return key_bit(mapping.first_key_index + (points_right ? 2U : 3U));
    }
    if (absolute_y >= tan_67_5 * absolute_x) {
        return key_bit(mapping.first_key_index + (points_up ? 0U : 1U));
    }
    if (points_up) {
        return key_bit(mapping.first_key_index + (points_right ? 5U : 4U));
    }
    return key_bit(mapping.first_key_index + (points_right ? 6U : 7U));
}

} // namespace

key_mask map_keystroke_keys(const XINPUT_GAMEPAD& gamepad) noexcept {
    key_mask result{};

    constexpr std::array<WORD, 14U> button_masks{
        XINPUT_GAMEPAD_DPAD_UP,
        XINPUT_GAMEPAD_DPAD_DOWN,
        XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_START,
        XINPUT_GAMEPAD_BACK,
        XINPUT_GAMEPAD_LEFT_THUMB,
        XINPUT_GAMEPAD_RIGHT_THUMB,
        XINPUT_GAMEPAD_LEFT_SHOULDER,
        XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_A,
        XINPUT_GAMEPAD_B,
        XINPUT_GAMEPAD_X,
        XINPUT_GAMEPAD_Y,
    };

    for (std::uint32_t index = 0U; index < button_masks.size(); ++index) {
        if ((gamepad.wButtons & button_masks[index]) != 0U) {
            result |= key_bit(index);
        }
    }

    if (gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        result |= key_bit(14U);
    }
    if (gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) {
        result |= key_bit(15U);
    }

    result |= map_thumbstick({.x = gamepad.sThumbLX, .y = gamepad.sThumbLY},
        {.deadzone = XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE, .first_key_index = 16U});
    result |= map_thumbstick({.x = gamepad.sThumbRX, .y = gamepad.sThumbRY},
        {.deadzone = XINPUT_GAMEPAD_RIGHT_THUMB_DEADZONE, .first_key_index = 24U});
    return result;
}

} // namespace inputnext::xinput::detail
