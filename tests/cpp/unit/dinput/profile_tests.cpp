#include "dinput/profile.hpp"

#include <catch2/catch_test_macros.hpp>

#include <dinput.h>
#include <inputnext/gamepad_state.hpp>

#include <algorithm>
#include <array>
#include <limits>

using namespace inputnext;
using namespace inputnext::dinput;

TEST_CASE("dinput.profile.exposes_stable_canonical_objects", "[unit][dinput]") {
    const auto profile = objects();
    REQUIRE(profile.size() == normalized_object_count);

    CHECK(profile[0].guid.Data1 == 0xA36D02E0U);
    CHECK(profile[0].id == (DIDFT_ABSAXIS | DIDFT_MAKEINSTANCE(0U)));
    CHECK(profile[0].native_offset == DIJOFS_X);
    CHECK(profile[0].kind == object_kind::axis);
    CHECK(profile[0].source_index == 0U);

    CHECK(profile[4].guid.Data1 == 0xA36D02F5U);
    CHECK(profile[5].guid.Data1 == 0xA36D02E3U);
    CHECK(profile[6].id == (DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(0U)));
    CHECK(profile[6].usage_page == 0x09U);
    CHECK(profile[6].usage == 1U);
    CHECK(profile[20].source_index == 14U);
    CHECK(profile[21].kind == object_kind::pov);
    CHECK(profile[21].native_offset == DIJOFS_POV(0));
}

TEST_CASE("dinput.profile.maps_axes_buttons_triggers_and_pov", "[unit][dinput]") {
    gamepad_state source{};
    source.buttons = supported_button_mask;
    source.left_stick = {.x = -1.0F, .y = 0.5F};
    source.right_stick = {.x = 0.25F, .y = -1.0F};
    source.left_trigger = 0.25F;
    source.right_trigger = 1.0F;

    const std::array<axis_properties, 6> properties{};
    const auto value = project(source, properties);
    CHECK(value.axes[0] == 0);
    CHECK(value.axes[1] == 16384);
    CHECK(value.axes[2] == 16384);
    CHECK(value.axes[3] == 40959);
    CHECK(value.axes[4] == 65535);
    CHECK(value.axes[5] == 65535);
    CHECK(std::ranges::all_of(value.buttons, [](BYTE button) { return button == 0x80U; }));
    CHECK(value.pov == 0xffffffffU);

    source.buttons = static_cast<button_mask>(gamepad_button::dpad_up) |
                     static_cast<button_mask>(gamepad_button::dpad_right);
    CHECK(project(source, properties).pov == 4500U);
    source.buttons = static_cast<button_mask>(gamepad_button::dpad_down) |
                     static_cast<button_mask>(gamepad_button::dpad_left);
    CHECK(project(source, properties).pov == 22500U);
}

TEST_CASE("dinput.profile.ranges_deadzone_and_saturation_have_expected_neutral_points",
    "[unit][dinput]") {
    std::array<axis_properties, 6> properties{};
    properties[0] = axis_properties{.minimum = -100, .maximum = 300};
    properties[1] =
        axis_properties{.minimum = 100, .maximum = 900, .deadzone = 1000, .saturation = 8000};
    properties[2] =
        axis_properties{.minimum = 100, .maximum = 500, .deadzone = 2000, .saturation = 8000};
    properties[3] = axis_properties{.minimum = 10, .maximum = 20, .saturation = 0};
    properties[5] = axis_properties{.minimum = 100, .maximum = 200, .saturation = 0};

    gamepad_state source{};
    source.left_stick = {.x = 0.0F, .y = 0.0F};
    source.left_trigger = 0.52F;
    auto value = project(source, properties);
    CHECK(value.axes[0] == 100);
    CHECK(value.axes[1] == 500);
    CHECK(value.axes[2] == 300);
    CHECK(value.axes[3] == 15);
    CHECK(value.axes[5] == 100);

    source.left_stick.x = 1.0F;
    source.left_stick.y = 0.1F;
    source.left_trigger = 0.2F;
    source.right_stick.x = 0.01F;
    source.right_trigger = 0.01F;
    value = project(source, properties);
    CHECK(value.axes[0] == 300);
    CHECK(value.axes[1] == 500);
    CHECK(value.axes[2] == 100);
    CHECK(value.axes[3] == 20);
    CHECK(value.axes[5] == 200);

    source.left_trigger = 1.0F;
    value = project(source, properties);
    CHECK(value.axes[2] == 500);
}

TEST_CASE("dinput.profile.invalid_samples_project_to_neutral_values", "[unit][dinput]") {
    gamepad_state source{};
    source.left_stick.x = std::numeric_limits<float>::quiet_NaN();
    std::array<axis_properties, 6> properties{};
    properties[2] = axis_properties{.minimum = 100, .maximum = 500};

    const auto value = project(source, properties);
    CHECK(value.axes[0] == 32767);
    CHECK(value.axes[2] == 100);
    CHECK(std::ranges::all_of(value.buttons, [](BYTE button) { return button == 0U; }));
    CHECK(value.pov == 0xffffffffU);
}
