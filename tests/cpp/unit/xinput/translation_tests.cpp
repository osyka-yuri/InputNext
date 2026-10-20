#include "xinput/translation.hpp"

#include <inputnext/gamepad_state.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace inputnext;

TEST_CASE("XInput stick and trigger conversion preserves endpoint and truncation rules",
    "[unit][xinput]") {
    CHECK(xinput::detail::to_xinput_axis(-1.0F) == -32768);
    CHECK(xinput::detail::to_xinput_axis(1.0F) == 32767);
    CHECK(xinput::detail::to_xinput_axis(-0.5F) == -16384);
    CHECK(xinput::detail::to_xinput_axis(0.5F) == 16383);
    CHECK(xinput::detail::to_xinput_trigger(0.0F) == 0U);
    CHECK(xinput::detail::to_xinput_trigger(1.0F) == 255U);
    CHECK(xinput::detail::to_xinput_trigger(0.5F) == 127U);
}

TEST_CASE("XInput button projection excludes Guide unless requested", "[unit][xinput]") {
    const auto supported = static_cast<button_mask>(0x3FFFU);
    const auto guide = static_cast<button_mask>(gamepad_button::guide);
    CHECK(xinput::detail::to_xinput_buttons(supported, false) == 0xF3FFU);
    CHECK(xinput::detail::to_xinput_buttons(supported | guide, false) == 0xF3FFU);
    CHECK(xinput::detail::to_xinput_buttons(supported | guide, true) == 0xF7FFU);
}

TEST_CASE("state translation compares fields and not structure padding", "[unit][xinput]") {
    gamepad_state source{};
    source.buttons = static_cast<button_mask>(gamepad_button::south) |
                     static_cast<button_mask>(gamepad_button::guide);
    source.left_stick = {.x = -1.0F, .y = 0.5F};
    source.right_stick = {.x = 1.0F, .y = -0.5F};
    source.left_trigger = 0.5F;
    source.right_trigger = 1.0F;

    XINPUT_GAMEPAD without_guide{};
    XINPUT_GAMEPAD with_guide{};
    xinput::detail::translate_state(source, without_guide, false);
    xinput::detail::translate_state(source, with_guide, true);

    CHECK(without_guide.wButtons == 0x1000U);
    CHECK(with_guide.wButtons == 0x1400U);
    CHECK(without_guide.sThumbLX == -32768);
    CHECK(without_guide.sThumbLY == 16383);
    CHECK(without_guide.sThumbRX == 32767);
    CHECK(without_guide.sThumbRY == -16384);
    CHECK(without_guide.bLeftTrigger == 127U);
    CHECK(without_guide.bRightTrigger == 255U);
    CHECK(xinput::detail::same_gamepad(without_guide, without_guide));
    CHECK_FALSE(xinput::detail::same_gamepad(without_guide, with_guide));
}
