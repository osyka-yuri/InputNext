#include "dinput/profile.hpp"
#include "values/scoped_mxcsr.hpp"

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>

#include <dinput.h>
#include <inputnext/gamepad_state.hpp>
#include <pmmintrin.h>
#include <xmmintrin.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <ranges>
#include <string_view>

using namespace inputnext;
using namespace inputnext::dinput;

namespace {

constexpr device_model standard_model{};

} // namespace

TEST_CASE("dinput.profile.exposes_stable_canonical_objects", "[unit][dinput]") {
    const auto profile = objects();
    REQUIRE(profile.size() == 22U);

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
    const auto value = standard_model.project(source, properties);
    CHECK(value.axes[0] == 0);
    CHECK(value.axes[1] == 16384);
    CHECK(value.axes[2] == 16384);
    CHECK(value.axes[3] == 40959);
    CHECK(value.axes[4] == 65535);
    CHECK(value.axes[5] == 65535);
    CHECK(std::ranges::all_of(value.buttons | std::views::take(standard_button_count),
        [](BYTE button) { return button == 0x80U; }));
    CHECK(value.buttons[15] == 0U);
    CHECK(value.buttons[16] == 0U);
    CHECK(value.pov == 0xffffffffU);

    source.buttons = static_cast<button_mask>(gamepad_button::dpad_up) |
                     static_cast<button_mask>(gamepad_button::dpad_right);
    CHECK(standard_model.project(source, properties).pov == 4500U);
    source.buttons = static_cast<button_mask>(gamepad_button::dpad_down) |
                     static_cast<button_mask>(gamepad_button::dpad_left);
    CHECK(standard_model.project(source, properties).pov == 22500U);
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
    auto value = standard_model.project(source, properties);
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
    value = standard_model.project(source, properties);
    CHECK(value.axes[0] == 300);
    CHECK(value.axes[1] == 500);
    CHECK(value.axes[2] == 100);
    CHECK(value.axes[3] == 20);
    CHECK(value.axes[5] == 200);

    source.left_trigger = 1.0F;
    value = standard_model.project(source, properties);
    CHECK(value.axes[2] == 500);
}

TEST_CASE("dinput.profile.invalid_samples_project_to_neutral_values", "[unit][dinput]") {
    gamepad_state source{};
    source.left_stick.x = std::numeric_limits<float>::quiet_NaN();
    std::array<axis_properties, 6> properties{};
    properties[2] = axis_properties{.minimum = 100, .maximum = 500};

    const auto value = standard_model.project(source, properties);
    CHECK(value.axes[0] == 32767);
    CHECK(value.axes[2] == 100);
    CHECK(std::ranges::all_of(value.buttons, [](BYTE button) { return button == 0U; }));
    CHECK(value.pov == 0xffffffffU);
}

TEST_CASE("dinput.profile.extended_model_exposes_digital_trigger_buttons", "[unit][dinput]") {
    const device_model model{true};
    const auto standard = objects();
    const auto extended = model.objects();

    REQUIRE(standard.size() == 22U);
    REQUIRE(extended.size() == 24U);

    for (std::size_t index = 0U; index < standard.size(); ++index) {
        CHECK(extended[index].guid == standard[index].guid);
        CHECK(extended[index].id == standard[index].id);
        CHECK(extended[index].native_offset == standard[index].native_offset);
        CHECK(extended[index].kind == standard[index].kind);
        CHECK(extended[index].source_index == standard[index].source_index);
    }

    CHECK(extended[22].guid.Data1 == 0xA36D02F0U);
    CHECK(extended[22].id == (DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(15U)));
    CHECK(extended[22].native_offset == static_cast<DWORD>(DIJOFS_BUTTON(15)));
    CHECK(extended[22].usage_page == 0x09U);
    CHECK(extended[22].usage == 16U);
    CHECK(std::wstring_view{extended[22].name} == L"LT");
    CHECK(extended[22].kind == object_kind::button);
    CHECK(extended[22].source_index == 15U);

    CHECK(extended[23].guid.Data1 == 0xA36D02F0U);
    CHECK(extended[23].id == (DIDFT_PSHBUTTON | DIDFT_MAKEINSTANCE(16U)));
    CHECK(extended[23].native_offset == static_cast<DWORD>(DIJOFS_BUTTON(16)));
    CHECK(extended[23].usage_page == 0x09U);
    CHECK(extended[23].usage == 17U);
    CHECK(std::wstring_view{extended[23].name} == L"RT");
    CHECK(extended[23].kind == object_kind::button);
    CHECK(extended[23].source_index == 16U);
}

TEST_CASE("dinput.profile.digital_triggers_project_with_threshold_and_preserve_analog",
    "[unit][dinput]") {
    const std::array<axis_properties, 6> properties{};
    const device_model default_model{true, 0.5F};

    gamepad_state source{};
    source.left_trigger = 0.499F;
    source.right_trigger = 0.5F;

    auto value = default_model.project(source, properties);
    CHECK(value.buttons[15] == 0U);
    CHECK(value.buttons[16] == 0x80U);
    CHECK(value.axes[2] == 32702);
    CHECK(value.axes[5] == 32768);

    source.left_trigger = 0.500F;
    source.right_trigger = 0.499F;
    value = default_model.project(source, properties);
    CHECK(value.buttons[15] == 0x80U);
    CHECK(value.buttons[16] == 0U);

    SECTION("extreme thresholds") {
        const device_model low_model{true, 0.01F};
        source.left_trigger = 0.009F;
        source.right_trigger = 0.010F;
        value = low_model.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0x80U);

        const device_model high_model{true, 1.0F};
        source.left_trigger = 0.999F;
        source.right_trigger = 1.0F;
        value = high_model.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0x80U);

        const device_model tiny_model{true, 1e-100};
        source.left_trigger = 0.0F;
        source.right_trigger = 0.0F;
        value = tiny_model.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0U);

        source.left_trigger = 0.01F;
        value = tiny_model.project(source, properties);
        CHECK(value.buttons[15] == 0x80U);
        CHECK(value.buttons[16] == 0U);
    }

    SECTION("float-inexact decimal thresholds match source float values at boundary") {
        const device_model model_42{true, 0.42};
        source.left_trigger = 0.419F;
        source.right_trigger = 0.42F;
        value = model_42.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0x80U);

        const device_model model_01{true, 0.01};
        source.left_trigger = 0.009F;
        source.right_trigger = 0.01F;
        value = model_01.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0x80U);

        const device_model model_80{true, 0.8};
        source.left_trigger = 0.799F;
        source.right_trigger = 0.8F;
        value = model_80.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0x80U);
    }

    SECTION("independence of shoulder buttons and triggers") {
        source = gamepad_state{};
        source.buttons = static_cast<button_mask>(gamepad_button::left_shoulder);
        source.left_trigger = 0.0F;
        value = default_model.project(source, properties);
        CHECK(value.buttons[4] == 0x80U);
        CHECK(value.buttons[15] == 0U);

        source.left_trigger = 0.8F;
        value = default_model.project(source, properties);
        CHECK(value.buttons[4] == 0x80U);
        CHECK(value.buttons[15] == 0x80U);

        source.buttons = static_cast<button_mask>(gamepad_button::right_shoulder);
        source.right_trigger = 0.0F;
        value = default_model.project(source, properties);
        CHECK(value.buttons[5] == 0x80U);
        CHECK(value.buttons[16] == 0U);

        source.right_trigger = 0.9F;
        value = default_model.project(source, properties);
        CHECK(value.buttons[5] == 0x80U);
        CHECK(value.buttons[16] == 0x80U);

        source.left_trigger = 0.7F;
        source.right_trigger = 0.7F;
        value = default_model.project(source, properties);
        CHECK(value.buttons[15] == 0x80U);
        CHECK(value.buttons[16] == 0x80U);
    }

    SECTION("invalid state clears digital triggers") {
        source = gamepad_state{};
        source.left_trigger = std::numeric_limits<float>::quiet_NaN();
        source.right_trigger = 1.0F;
        value = default_model.project(source, properties);
        CHECK(value.buttons[15] == 0U);
        CHECK(value.buttons[16] == 0U);
    }
}

TEST_CASE("dinput.profile.digital_threshold_is_independent_of_mxcsr", "[unit][dinput]") {
    struct threshold_case final {
        double configured{};
        float below{};
        float at_threshold{};
    };

    const std::array cases{
        threshold_case{.configured = 1e-310, .at_threshold = std::numeric_limits<float>::min()},
        threshold_case{.configured = 1e-40, .at_threshold = std::numeric_limits<float>::min()},
        threshold_case{.configured = 0.7,
            .below = std::nextafter(0.7F, 0.0F),
            .at_threshold = 0.7F},
        threshold_case{.configured = 0x1.000001p-1, .below = 0x1.fffffep-2F, .at_threshold = 0.5F},
        threshold_case{.configured = 0x1.000003p-1,
            .below = 0x1.000002p-1F,
            .at_threshold = 0x1.000004p-1F},
        threshold_case{.configured = 0x1.ffffffp-1, .below = 0x1.fffffep-1F, .at_threshold = 1.0F},
    };
    constexpr std::array<unsigned int, 4U> rounding_modes{_MM_ROUND_NEAREST,
        _MM_ROUND_DOWN,
        _MM_ROUND_UP,
        _MM_ROUND_TOWARD_ZERO};
    const auto mxcsr = (_mm_getcsr() & ~static_cast<unsigned int>(_MM_ROUND_MASK)) |
                       _MM_DENORMALS_ZERO_ON | _MM_FLUSH_ZERO_ON;

    for (const auto& scenario : cases) {
        for (const auto rounding : rounding_modes) {
            CAPTURE(scenario.configured, rounding);
            const auto value = [&] {
                const test::scoped_mxcsr control{mxcsr | rounding};
                const device_model model{true, scenario.configured};
                return model.project(gamepad_state{.left_trigger = scenario.below,
                                         .right_trigger = scenario.at_threshold},
                    {});
            }();
            CHECK(value.buttons[15] == 0U);
            CHECK(value.buttons[16] == 0x80U);
        }
    }
}
