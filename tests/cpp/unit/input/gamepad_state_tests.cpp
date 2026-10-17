#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <array>
#include <cstdint>
#include <limits>

using namespace inputnext;

TEST_CASE("input.latest.gamepad_state_accepts_original_button_bits", "[unit][input]") {
    constexpr auto all_original_buttons =
        static_cast<std::uint32_t>(gamepad_button::dpad_up) |
        static_cast<std::uint32_t>(gamepad_button::dpad_down) |
        static_cast<std::uint32_t>(gamepad_button::dpad_left) |
        static_cast<std::uint32_t>(gamepad_button::dpad_right) |
        static_cast<std::uint32_t>(gamepad_button::menu) |
        static_cast<std::uint32_t>(gamepad_button::view) |
        static_cast<std::uint32_t>(gamepad_button::left_stick) |
        static_cast<std::uint32_t>(gamepad_button::right_stick) |
        static_cast<std::uint32_t>(gamepad_button::left_shoulder) |
        static_cast<std::uint32_t>(gamepad_button::right_shoulder) |
        static_cast<std::uint32_t>(gamepad_button::south) |
        static_cast<std::uint32_t>(gamepad_button::east) |
        static_cast<std::uint32_t>(gamepad_button::west) |
        static_cast<std::uint32_t>(gamepad_button::north) |
        static_cast<std::uint32_t>(gamepad_button::guide);
    STATIC_REQUIRE(all_original_buttons == supported_button_mask);

    auto state = gamepad_state{};
    state.buttons = GENERATE(0U,
        static_cast<std::uint32_t>(gamepad_button::dpad_up),
        static_cast<std::uint32_t>(gamepad_button::guide),
        supported_button_mask);

    CHECK(is_valid_gamepad_state(state));
}

TEST_CASE("input.latest.gamepad_state_rejects_unsupported_button_bits", "[unit][input]") {
    auto state = gamepad_state{};
    state.buttons = GENERATE(supported_button_mask + 1U, 1U << 31U);

    CHECK_FALSE(is_valid_gamepad_state(state));
}

TEST_CASE("input.latest.gamepad_state_accepts_axis_and_trigger_boundaries", "[unit][input]") {
    const auto axis = GENERATE(-1.0F, 0.0F, 1.0F);
    const auto trigger = GENERATE(0.0F, 0.5F, 1.0F);
    const gamepad_state state{
        .buttons = supported_button_mask,
        .left_stick = {.x = axis, .y = -axis},
        .right_stick = {.x = -axis, .y = axis},
        .left_trigger = trigger,
        .right_trigger = 1.0F - trigger,
    };

    CHECK(is_valid_gamepad_state(state));
}

TEST_CASE("input.latest.gamepad_state_rejects_invalid_float_fields", "[unit][input]") {
    const auto field = GENERATE(0U, 1U, 2U, 3U, 4U, 5U);
    const auto value = GENERATE(-1.01F,
        1.01F,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity());
    auto state = gamepad_state{};
    const std::array<float*, 6U> float_fields{
        &state.left_stick.x,
        &state.left_stick.y,
        &state.right_stick.x,
        &state.right_stick.y,
        &state.left_trigger,
        &state.right_trigger,
    };
    *float_fields[field] = value;
    CAPTURE(field, value);

    CHECK_FALSE(is_valid_gamepad_state(state));
}
