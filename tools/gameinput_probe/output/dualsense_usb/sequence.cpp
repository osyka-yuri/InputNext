#include "gameinput_probe/output/dualsense_usb/sequence.hpp"

#include <inputnext/output.hpp>

#include <array>
#include <span>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] constexpr output::trigger_value feedback_uniform() noexcept {
    return {.mode = output::trigger_mode::feedback,
        .layout = output::trigger_layout::uniform,
        .start = 0.2F,
        .end = 1.0F,
        .strength = 0.75F};
}

[[nodiscard]] constexpr output::trigger_value feedback_per_zone() noexcept {
    return {.mode = output::trigger_mode::feedback,
        .layout = output::trigger_layout::per_zone,
        .zones = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U, 0U, 0U}};
}

[[nodiscard]] constexpr output::trigger_value weapon() noexcept {
    return {.mode = output::trigger_mode::weapon,
        .layout = output::trigger_layout::uniform,
        .start = 0.3F,
        .end = 0.8F,
        .strength = 0.75F};
}

[[nodiscard]] constexpr output::trigger_value vibration_uniform() noexcept {
    return {.mode = output::trigger_mode::vibration,
        .layout = output::trigger_layout::uniform,
        .start = 0.1F,
        .end = 1.0F,
        .strength = 0.625F,
        .frequency = 120.0F};
}

[[nodiscard]] constexpr output::trigger_value vibration_per_zone() noexcept {
    return {.mode = output::trigger_mode::vibration,
        .layout = output::trigger_layout::per_zone,
        .frequency = 90.0F,
        .zones = {8U, 7U, 6U, 5U, 4U, 3U, 2U, 1U, 0U, 0U}};
}

[[nodiscard]] constexpr dualsense_usb_step trigger_step(const std::string_view name,
    const std::string_view instruction,
    const bool left,
    const output::trigger_value trigger) noexcept {
    output::channel_values values{};
    if (left) {
        values.left_trigger = trigger;
    } else {
        values.right_trigger = trigger;
    }
    return {.name = name, .values = values, .instruction = instruction};
}

constexpr std::array sequence{
    dualsense_usb_step{.name = "lightbar_rgb",
        .values = {.leds = {.rgb = {0xFFU, 0x40U, 0x00U}}},
        .instruction = "Check that the lightbar is orange."},
    dualsense_usb_step{.name = "player_leds",
        .values = {.leds = {.player_leds = 3U}},
        .instruction =
            "Lightbar off. Check the white dots below the touchpad: three should be lit."},
    dualsense_usb_step{.name = "microphone_led_on",
        .values = {.leds = {.microphone_led = true}},
        .instruction =
            "Lightbar off. Check the amber LED on the microphone mute button: it should be on."},
    dualsense_usb_step{.name = "microphone_led_off",
        .values = {},
        .instruction = "Check the amber LED on the microphone mute button: it should be off. The "
                       "lightbar stays off."},
    dualsense_usb_step{.name = "rumble_low_motor",
        .values = {.rumble = {.low_frequency = 0.65F}},
        .instruction = "Feel the low-frequency vibration."},
    dualsense_usb_step{.name = "rumble_high_motor",
        .values = {.rumble = {.high_frequency = 0.65F}},
        .instruction = "Feel the high-frequency vibration."},
    dualsense_usb_step{.name = "rumble_both_motors",
        .values = {.rumble = {.low_frequency = 0.4F, .high_frequency = 0.4F}},
        .instruction = "Feel both vibration motors together."},
    trigger_step("left_feedback_uniform",
        "Pull the left trigger: feel steady resistance.",
        true,
        feedback_uniform()),
    trigger_step("left_feedback_per_zone",
        "Pull the left trigger slowly: feel resistance change with travel.",
        true,
        feedback_per_zone()),
    trigger_step("left_weapon",
        "Pull the left trigger through the resistant section.",
        true,
        weapon()),
    trigger_step("left_vibration_uniform",
        "Pull the left trigger: feel vibration.",
        true,
        vibration_uniform()),
    trigger_step("left_vibration_per_zone",
        "Pull the left trigger slowly: feel vibration change with travel.",
        true,
        vibration_per_zone()),
    trigger_step("left_trigger_off",
        "Check that the left trigger moves freely without vibration.",
        true,
        {}),
    trigger_step("right_feedback_uniform",
        "Pull the right trigger: feel steady resistance.",
        false,
        feedback_uniform()),
    trigger_step("right_feedback_per_zone",
        "Pull the right trigger slowly: feel resistance change with travel.",
        false,
        feedback_per_zone()),
    trigger_step("right_weapon",
        "Pull the right trigger through the resistant section.",
        false,
        weapon()),
    trigger_step("right_vibration_uniform",
        "Pull the right trigger: feel vibration.",
        false,
        vibration_uniform()),
    trigger_step("right_vibration_per_zone",
        "Pull the right trigger slowly: feel vibration change with travel.",
        false,
        vibration_per_zone()),
    trigger_step("right_trigger_off",
        "Check that the right trigger moves freely without vibration.",
        false,
        {}),
    dualsense_usb_step{.name = "final_neutral",
        .values = {},
        .instruction = "Check that vibration, trigger effects and diagnostic LEDs are off."},
};

} // namespace

std::span<const dualsense_usb_step> dualsense_usb_steps() noexcept {
    return sequence;
}

output::channel_values dualsense_usb_neutral_values() noexcept {
    return {};
}

} // namespace inputnext::gameinput_detail
