#pragma once

#include <cmath>
#include <cstdint>

namespace inputnext {

enum class gamepad_button : std::uint16_t {
    dpad_up = 1U << 0U,
    dpad_down = 1U << 1U,
    dpad_left = 1U << 2U,
    dpad_right = 1U << 3U,
    menu = 1U << 4U,
    view = 1U << 5U,
    left_stick = 1U << 6U,
    right_stick = 1U << 7U,
    left_shoulder = 1U << 8U,
    right_shoulder = 1U << 9U,
    south = 1U << 10U,
    east = 1U << 11U,
    west = 1U << 12U,
    north = 1U << 13U,
    guide = 1U << 14U,
};

using button_mask = std::uint32_t;
inline constexpr button_mask supported_button_mask = (button_mask{1} << 15U) - 1U;

struct stick_state final {
    float x{};
    float y{};

    friend constexpr bool operator==(const stick_state&, const stick_state&) noexcept = default;
};

struct gamepad_state final {
    button_mask buttons{};
    stick_state left_stick{};
    stick_state right_stick{};
    float left_trigger{};
    float right_trigger{};

    friend constexpr bool operator==(const gamepad_state&, const gamepad_state&) noexcept = default;
};

[[nodiscard]] inline bool is_valid_gamepad_state(const gamepad_state& state) noexcept {
    const auto valid_axis = [](float value) noexcept {
        return std::isfinite(value) && value >= -1.0F && value <= 1.0F;
    };
    const auto valid_trigger = [](float value) noexcept {
        return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
    };

    return (state.buttons & ~supported_button_mask) == 0U && valid_axis(state.left_stick.x) &&
           valid_axis(state.left_stick.y) && valid_axis(state.right_stick.x) &&
           valid_axis(state.right_stick.y) && valid_trigger(state.left_trigger) &&
           valid_trigger(state.right_trigger);
}

} // namespace inputnext
