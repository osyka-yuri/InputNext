#include "gameinput/dualsense/usb_report.hpp"

#include "gameinput/dualsense/feedback_payload.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace {

using namespace inputnext::gameinput_detail::dualsense;
namespace output = inputnext::output;
using output::trigger_layout;
using output::trigger_mode;
using output::trigger_value;

[[nodiscard]] constexpr std::byte byte(unsigned int value) noexcept {
    return static_cast<std::byte>(value);
}

[[nodiscard]] output::channel_values golden_values() {
    output::channel_values values{};
    values.rumble.low_frequency = 0.5F;
    values.rumble.high_frequency = 1.0F;
    values.right_trigger = {
        .mode = output::trigger_mode::weapon,
        .start = 0.33F,
        .end = 0.77F,
        .strength = 1.0F,
    };
    values.left_trigger = {
        .mode = output::trigger_mode::feedback,
        .start = 0.22F,
        .end = std::numeric_limits<float>::quiet_NaN(),
        .strength = 0.5F,
    };
    values.leds = {
        .rgb = {255U, 128U, 64U},
        .player_leds = 1U,
        .microphone_led = true,
    };
    return values;
}

[[nodiscard]] constexpr unsigned int byte_value(std::byte value) noexcept {
    return std::to_integer<unsigned int>(value);
}

void check_trigger(const usb_report& report,
    std::size_t mode_offset,
    std::uint8_t expected_mode,
    const std::array<std::uint8_t, 10U>& expected_parameters) {
    CHECK(byte_value(report[mode_offset]) == expected_mode);
    for (std::size_t index = 0U; index < expected_parameters.size(); ++index) {
        CHECK(byte_value(report[mode_offset + 1U + index]) == expected_parameters[index]);
    }
}

void check_right_trigger_off(const output::trigger_value& trigger) {
    output::channel_values values{};
    values.right_trigger = trigger;
    check_trigger(encode_usb_report(values), 11U, 0x05U, {});
}

[[nodiscard]] trigger_value feedback_trigger(float start, float strength) {
    return {
        .mode = trigger_mode::feedback,
        .start = start,
        .strength = strength,
    };
}

[[nodiscard]] trigger_value weapon_trigger(float start,
    float end,
    float strength,
    trigger_layout layout = trigger_layout::uniform) {
    return {
        .mode = trigger_mode::weapon,
        .layout = layout,
        .start = start,
        .end = end,
        .strength = strength,
    };
}

[[nodiscard]] trigger_value vibration_trigger(float frequency) {
    return {
        .mode = trigger_mode::vibration,
        .start = 0.5F,
        .strength = 0.5F,
        .frequency = frequency,
    };
}

TEST_CASE("gameinput.dualsense.usb_report_golden_vector", "[gameinput][dualsense][unit]") {
    constexpr std::array<std::uint8_t, 48U> compact_golden{
        0x02U,
        0x0FU,
        0x15U,
        0xFFU,
        0x80U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x01U,
        0x00U,
        0x25U,
        0x88U,
        0x00U,
        0x07U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x21U,
        0xFCU,
        0x03U,
        0xC0U,
        0xB6U,
        0x6DU,
        0x1BU,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x00U,
        0x04U,
        0xFFU,
        0x80U,
        0x40U,
    };

    usb_report expected{};
    for (std::size_t index = 0U; index < compact_golden.size(); ++index) {
        expected[index] = byte(compact_golden[index]);
    }

    const usb_report report = encode_usb_report(golden_values());
    const feedback_payload payload = encode_feedback_payload(golden_values());
    CHECK((report == expected));
    CHECK(report.size() == 64U);
    for (std::size_t index = 0U; index < payload.size(); ++index) {
        CHECK(byte_value(payload[index]) == compact_golden[index + 1U]);
    }
    for (std::size_t index = 48U; index < report.size(); ++index) {
        CHECK(byte_value(report[index]) == 0U);
    }
}

TEST_CASE("gameinput.dualsense.hd_route_changes_only_mode_and_motor_bytes",
    "[gameinput][dualsense][unit]") {
    const auto values = golden_values();
    const auto classic = encode_usb_report(values);
    const auto hd_report = encode_usb_report(values, output::body_actuator_route::hd);

    CHECK(classic.size() == hd_report.size());
    CHECK(byte_value(classic[1U]) == 0x0FU);
    CHECK(byte_value(hd_report[1U]) == 0x0CU);
    CHECK(hd_report[2U] == classic[2U]);
    CHECK(byte_value(hd_report[3U]) == 0U);
    CHECK(byte_value(hd_report[4U]) == 0U);
    for (std::size_t index = 5U; index < classic.size(); ++index) {
        CHECK(hd_report[index] == classic[index]);
    }

    const auto full = usb_report_payload(hd_report, 64U).value_or(std::span<const std::byte>{});
    const auto without_id =
        usb_report_payload(hd_report, 63U).value_or(std::span<const std::byte>{});
    const auto compact = usb_report_payload(hd_report, 48U).value_or(std::span<const std::byte>{});
    CHECK(full.size() == 64U);
    CHECK(without_id.size() == 63U);
    CHECK(compact.size() == 48U);
}

TEST_CASE("gameinput.dualsense.led_updates_do_not_release_led_control",
    "[gameinput][dualsense][unit]") {
    const std::array states{
        output::channel_values{},
        output::channel_values{.leds = {.rgb = {255U, 64U, 0U}}},
        output::channel_values{.leds = {.player_leds = 3U}},
        output::channel_values{.leds = {.microphone_led = true}},
    };
    for (const auto& state : states) {
        const auto report = encode_usb_report(state);
        CHECK((byte_value(report[2U]) & 0x08U) == 0U);
        CHECK((byte_value(report[2U]) & 0x15U) == 0x15U);
    }
}

TEST_CASE("gameinput.dualsense.usb_report_payload_accepts_only_supported_capacities",
    "[gameinput][dualsense][unit]") {
    const usb_report report = encode_usb_report(golden_values());

    const auto full = usb_report_payload(report, 64U).value_or(std::span<const std::byte>{});
    REQUIRE(full.size() == 64U);
    CHECK(byte_value(full[0U]) == 0x02U);
    CHECK(byte_value(full[63U]) == 0U);

    const auto payload = usb_report_payload(report, 63U).value_or(std::span<const std::byte>{});
    REQUIRE(payload.size() == 63U);
    CHECK(byte_value(payload[0U]) == byte_value(report[1U]));
    CHECK(byte_value(payload[62U]) == byte_value(report[63U]));

    const auto compact = usb_report_payload(report, 48U).value_or(std::span<const std::byte>{});
    REQUIRE(compact.size() == 48U);
    CHECK(byte_value(compact[0U]) == byte_value(report[0U]));
    CHECK(byte_value(compact[47U]) == byte_value(report[47U]));

    constexpr std::array unsupported_capacities{0U, 1U, 46U, 47U, 49U, 62U, 65U, 128U};
    for (const std::size_t capacity : unsupported_capacities) {
        CHECK((!usb_report_payload(report, capacity).has_value()));
    }
}

TEST_CASE("gameinput.dualsense.usb_report_quantizes_rumble_and_handles_special_values",
    "[gameinput][dualsense][unit]") {
    output::channel_values values{};
    values.rumble.low_frequency = std::nextafter(0.5F / 255.0F, 0.0F);
    values.rumble.high_frequency = 0.5F / 255.0F;
    auto report = encode_usb_report(values);
    CHECK(byte_value(report[4U]) == 0U);
    CHECK(byte_value(report[3U]) == 1U);

    values.rumble.low_frequency = 0.5F;
    values.rumble.high_frequency = 0.5F;
    report = encode_usb_report(values);
    CHECK(byte_value(report[4U]) == 128U);
    CHECK(byte_value(report[3U]) == 128U);

    values.rumble.low_frequency = -0.25F;
    values.rumble.high_frequency = 1.25F;
    report = encode_usb_report(values);
    CHECK(byte_value(report[4U]) == 0U);
    CHECK(byte_value(report[3U]) == 255U);

    values.rumble.low_frequency = std::numeric_limits<float>::quiet_NaN();
    values.rumble.high_frequency = std::numeric_limits<float>::infinity();
    report = encode_usb_report(values);
    CHECK(byte_value(report[4U]) == 0U);
    CHECK(byte_value(report[3U]) == 255U);

    values.rumble.low_frequency = -std::numeric_limits<float>::infinity();
    values.rumble.high_frequency = 0.0F;
    report = encode_usb_report(values);
    CHECK(byte_value(report[4U]) == 0U);
    CHECK(byte_value(report[3U]) == 0U);
}

TEST_CASE("gameinput.dualsense.usb_report_encodes_led_offsets_and_rejects_raw_player_values",
    "[gameinput][dualsense][unit]") {
    constexpr std::array<std::uint8_t, 5U> patterns{0x00U, 0x04U, 0x0AU, 0x15U, 0x1BU};
    for (std::size_t player = 0U; player < patterns.size(); ++player) {
        output::channel_values values{};
        values.leds.player_leds = static_cast<std::uint8_t>(player);
        values.leds.rgb = {0x12U, 0x34U, 0x56U};
        values.leds.microphone_led = true;
        const auto report = encode_usb_report(values);

        CHECK(byte_value(report[9U]) == 1U);
        CHECK(byte_value(report[44U]) == patterns[player]);
        CHECK(byte_value(report[45U]) == 0x12U);
        CHECK(byte_value(report[46U]) == 0x34U);
        CHECK(byte_value(report[47U]) == 0x56U);
    }

    constexpr std::array<std::uint8_t, 2U> unsupported_players{5U, 0xFFU};
    for (const std::uint8_t unsupported_player : unsupported_players) {
        output::channel_values values{};
        values.leds.player_leds = unsupported_player;
        values.leds.microphone_led = false;
        const auto report = encode_usb_report(values);
        CHECK(byte_value(report[9U]) == 0U);
        CHECK(byte_value(report[44U]) == 0U);
    }
}

TEST_CASE("gameinput.dualsense.triggers_match_uniform_and_per_zone_vectors",
    "[gameinput][dualsense][unit]") {
    const usb_report uniform_report = encode_usb_report(golden_values());
    check_trigger(uniform_report, 11U, 0x25U, {0x88U, 0U, 7U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    check_trigger(uniform_report,
        22U,
        0x21U,
        {0xFCU, 0x03U, 0xC0U, 0xB6U, 0x6DU, 0x1BU, 0U, 0U, 0U, 0U});

    output::channel_values values{};
    values.right_trigger = {
        .mode = output::trigger_mode::feedback,
        .layout = output::trigger_layout::per_zone,
        .zones = {0U, 0U, 1U, 2U, 3U, 5U, 6U, 7U, 8U, 8U},
    };
    values.left_trigger = {
        .mode = output::trigger_mode::vibration,
        .layout = output::trigger_layout::per_zone,
        .frequency = 120.0F,
        .zones = {8U, 0U, 8U, 0U, 8U, 0U, 8U, 0U, 8U, 0U},
    };
    const usb_report per_zone_report = encode_usb_report(values);
    check_trigger(per_zone_report,
        11U,
        0x21U,
        {0xFCU, 0x03U, 0x00U, 0x22U, 0xD6U, 0x3FU, 0U, 0U, 0U, 0U});
    check_trigger(per_zone_report,
        22U,
        0x26U,
        {0x55U, 0x01U, 0xC7U, 0x71U, 0x1CU, 0x07U, 0U, 0U, 120U, 0U});
}

TEST_CASE("gameinput.dualsense.trigger_quantization_rounds_half_away_from_zero",
    "[gameinput][dualsense][unit]") {
    output::channel_values values{};
    values.right_trigger = {
        .mode = output::trigger_mode::feedback,
        .start = 0.5F,
        .strength = 0.5F,
    };
    check_trigger(encode_usb_report(values),
        11U,
        0x21U,
        {0xE0U, 0x03U, 0x00U, 0x80U, 0x6DU, 0x1BU, 0U, 0U, 0U, 0U});

    values.right_trigger.strength = 0.0625F;
    check_trigger(encode_usb_report(values),
        11U,
        0x21U,
        {0xE0U, 0x03U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});
    values.right_trigger.strength = std::nextafter(0.0625F, 0.0F);
    check_right_trigger_off(values.right_trigger);

    values.right_trigger = {
        .mode = output::trigger_mode::feedback,
        .start = 0.0F,
        .strength = 1.0F,
    };
    check_trigger(encode_usb_report(values),
        11U,
        0x21U,
        {0xFFU, 0x03U, 0xFFU, 0xFFU, 0xFFU, 0x3FU, 0U, 0U, 0U, 0U});

    values.right_trigger.start = 1.0F;
    check_trigger(encode_usb_report(values),
        11U,
        0x21U,
        {0x00U, 0x02U, 0x00U, 0x00U, 0x00U, 0x38U, 0U, 0U, 0U, 0U});

    values.right_trigger = {
        .mode = output::trigger_mode::weapon,
        .start = 0.5F,
        .end = 0.88F,
        .strength = 0.5F,
    };
    check_trigger(encode_usb_report(values),
        11U,
        0x25U,
        {0x20U, 0x01U, 0x03U, 0U, 0U, 0U, 0U, 0U, 0U, 0U});

    values.right_trigger = {
        .mode = output::trigger_mode::vibration,
        .layout = output::trigger_layout::per_zone,
        .frequency = 120.5F,
        .zones = {8U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
    };
    check_trigger(encode_usb_report(values),
        11U,
        0x26U,
        {0x01U, 0U, 7U, 0U, 0U, 0U, 0U, 0U, 121U, 0U});

    values.right_trigger.frequency = 1.0F;
    check_trigger(encode_usb_report(values),
        11U,
        0x26U,
        {0x01U, 0U, 7U, 0U, 0U, 0U, 0U, 0U, 1U, 0U});
    values.right_trigger.frequency = 255.0F;
    check_trigger(encode_usb_report(values),
        11U,
        0x26U,
        {0x01U, 0U, 7U, 0U, 0U, 0U, 0U, 0U, 255U, 0U});
}

TEST_CASE("gameinput.dualsense.invalid_trigger_inputs_degrade_to_off_with_zero_parameters",
    "[gameinput][dualsense][unit]") {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array invalid_feedback{
        feedback_trigger(nan, 0.5F),
        feedback_trigger(-0.01F, 0.5F),
        feedback_trigger(1.01F, 0.5F),
        feedback_trigger(0.5F, nan),
        feedback_trigger(0.5F, 1.01F),
        feedback_trigger(0.5F, 0.0F),
        trigger_value{
            .mode = trigger_mode::feedback,
            .layout = trigger_layout::per_zone,
            .zones = {},
        },
        trigger_value{
            .mode = trigger_mode::feedback,
            .layout = trigger_layout::per_zone,
            .zones = {0U, 0U, 9U, 0U, 0U, 0U, 0U, 0U, 0U, 0U},
        },
        trigger_value{
            .mode = trigger_mode::feedback,
            .layout = std::bit_cast<trigger_layout>(std::uint8_t{0xFFU}),
            .zones = {8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U},
        },
    };
    for (const auto& trigger : invalid_feedback) {
        check_right_trigger_off(trigger);
    }

    const std::array invalid_weapons{
        weapon_trigger(0.0F, 0.77F, 1.0F),
        weapon_trigger(0.11F, 0.77F, 0.5F),
        weapon_trigger(0.88F, 0.99F, 0.5F),
        weapon_trigger(0.33F, 1.0F, 1.0F),
        weapon_trigger(0.33F, 0.34F, 0.5F),
        weapon_trigger(0.5F, 0.5F, 0.5F),
        weapon_trigger(0.75F, 0.25F, 0.5F),
        weapon_trigger(0.33F, 0.77F, 0.0F),
        weapon_trigger(0.33F, 0.77F, 0.5F, trigger_layout::per_zone),
        weapon_trigger(nan, 0.75F, 0.5F),
        weapon_trigger(0.25F, infinity, 0.5F),
        weapon_trigger(0.25F, 0.75F, nan),
    };
    for (const auto& trigger : invalid_weapons) {
        check_right_trigger_off(trigger);
    }

    const std::array invalid_vibrations{
        vibration_trigger(0.0F),
        vibration_trigger(0.99F),
        vibration_trigger(256.0F),
        vibration_trigger(nan),
        vibration_trigger(infinity),
        vibration_trigger(-infinity),
        trigger_value{
            .mode = trigger_mode::vibration,
            .layout = trigger_layout::per_zone,
            .zones = {},
        },
        trigger_value{
            .mode = trigger_mode::vibration,
            .layout = trigger_layout::per_zone,
            .frequency = 120.0F,
            .zones = {0U, 0U, 0U, 0U, 0U, 0U, 9U, 0U, 0U, 0U},
        },
        trigger_value{
            .mode = trigger_mode::vibration,
            .layout = std::bit_cast<trigger_layout>(std::uint8_t{0xFFU}),
            .frequency = 120.0F,
            .zones = {8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U, 8U},
        },
    };
    for (const auto& trigger : invalid_vibrations) {
        check_right_trigger_off(trigger);
    }

    check_right_trigger_off(
        trigger_value{.mode = std::bit_cast<trigger_mode>(std::uint8_t{0xFFU})});
    check_right_trigger_off(trigger_value{.mode = trigger_mode::off});
}

} // namespace
