#include "feedback_payload.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_body.hpp>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace inputnext::gameinput_detail::dualsense {
namespace {

constexpr std::size_t flags0_offset = 0U;
constexpr std::size_t flags1_offset = 1U;
constexpr std::size_t right_motor_offset = 2U;
constexpr std::size_t left_motor_offset = 3U;
constexpr std::size_t microphone_led_offset = 8U;
constexpr std::size_t right_trigger_mode_offset = 10U;
constexpr std::size_t left_trigger_mode_offset = 21U;
constexpr std::size_t player_leds_offset = 43U;
constexpr std::size_t lightbar_red_offset = 44U;
constexpr std::size_t lightbar_green_offset = 45U;
constexpr std::size_t lightbar_blue_offset = 46U;

constexpr std::uint8_t classic_flags0 = 0x0FU;
constexpr std::uint8_t hd_flags0 = 0x0CU;
constexpr unsigned int microphone_led_control = 0x01U;
constexpr unsigned int lightbar_control = 0x04U;
constexpr unsigned int player_led_control = 0x10U;
constexpr std::uint8_t flags1 = microphone_led_control | lightbar_control | player_led_control;
constexpr std::uint8_t trigger_mode_off = 0x05U;
constexpr std::uint8_t trigger_mode_feedback = 0x21U;
constexpr std::uint8_t trigger_mode_weapon = 0x25U;
constexpr std::uint8_t trigger_mode_vibration = 0x26U;

struct trigger_encoding final {
    std::uint8_t mode{trigger_mode_off};
    std::array<std::uint8_t, 10U> parameters{};
};

[[nodiscard]] constexpr std::byte as_byte(std::uint8_t value) noexcept {
    return static_cast<std::byte>(value);
}

[[nodiscard]] std::uint8_t quantize_rumble(float value) noexcept {
    if (std::isnan(value) || value <= 0.0F) {
        return 0U;
    }
    if (value >= 1.0F) {
        return 255U;
    }

    const double scaled = static_cast<double>(value) * 255.0;
    return static_cast<std::uint8_t>(std::lround(scaled));
}

[[nodiscard]] bool quantize_position(float value, std::uint8_t& zone) noexcept {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        return false;
    }

    zone = static_cast<std::uint8_t>(std::lround(static_cast<double>(value) * 9.0));
    return true;
}

[[nodiscard]] bool quantize_strength(float value, std::uint8_t& level) noexcept {
    if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
        return false;
    }

    level = static_cast<std::uint8_t>(std::lround(static_cast<double>(value) * 8.0));
    return true;
}

[[nodiscard]] bool encode_zone_levels(const std::array<std::uint8_t, 10U>& levels,
    std::array<std::uint8_t, 10U>& parameters) noexcept {
    std::uint16_t active_zones = 0U;
    std::uint32_t packed_levels = 0U;

    for (std::size_t zone = 0U; zone < levels.size(); ++zone) {
        const std::uint8_t level = levels[zone];
        if (level > output::maximum_trigger_zone_level) {
            return false;
        }
        if (level != 0U) {
            active_zones |= static_cast<std::uint16_t>(1U << zone);
            packed_levels |= static_cast<std::uint32_t>(level - 1U) << (zone * 3U);
        }
    }

    if (active_zones == 0U) {
        return false;
    }

    parameters[0U] = static_cast<std::uint8_t>(active_zones & 0xFFU);
    parameters[1U] = static_cast<std::uint8_t>(active_zones >> 8U);
    for (std::size_t byte = 0U; byte < 4U; ++byte) {
        parameters[2U + byte] = static_cast<std::uint8_t>(packed_levels >> (byte * 8U));
    }
    return true;
}

[[nodiscard]] bool encode_uniform_feedback(const output::trigger_value& trigger,
    std::array<std::uint8_t, 10U>& parameters) noexcept {
    std::uint8_t start_zone = 0U;
    std::uint8_t strength_level = 0U;
    if (!quantize_position(trigger.start, start_zone) ||
        !quantize_strength(trigger.strength, strength_level) || strength_level == 0U) {
        return false;
    }

    std::array<std::uint8_t, 10U> levels{};
    for (std::size_t zone = start_zone; zone < levels.size(); ++zone) {
        levels[zone] = strength_level;
    }
    return encode_zone_levels(levels, parameters);
}

[[nodiscard]] bool encode_zonal_effect(const output::trigger_value& trigger,
    std::array<std::uint8_t, 10U>& parameters) noexcept {
    switch (trigger.layout) {
    case output::trigger_layout::uniform:
        return encode_uniform_feedback(trigger, parameters);
    case output::trigger_layout::per_zone:
        return encode_zone_levels(trigger.zones, parameters);
    default:
        return false;
    }
}

[[nodiscard]] trigger_encoding encode_trigger(const output::trigger_value& trigger) noexcept {
    trigger_encoding encoded{};

    switch (trigger.mode) {
    case output::trigger_mode::off:
        break;
    case output::trigger_mode::feedback:
        if (encode_zonal_effect(trigger, encoded.parameters)) {
            encoded.mode = trigger_mode_feedback;
        }
        break;
    case output::trigger_mode::weapon: {
        if (trigger.layout != output::trigger_layout::uniform) {
            break;
        }

        std::uint8_t start_zone = 0U;
        std::uint8_t end_zone = 0U;
        std::uint8_t strength_level = 0U;
        if (!quantize_position(trigger.start, start_zone) ||
            !quantize_position(trigger.end, end_zone) ||
            !quantize_strength(trigger.strength, strength_level) || trigger.end <= trigger.start ||
            strength_level == 0U) {
            break;
        }
        if (start_zone < 2U || start_zone > 7U || end_zone > 8U || end_zone <= start_zone) {
            break;
        }

        const auto active_zones = static_cast<std::uint16_t>((1U << start_zone) | (1U << end_zone));
        encoded.mode = trigger_mode_weapon;
        encoded.parameters[0U] = static_cast<std::uint8_t>(active_zones & 0xFFU);
        encoded.parameters[1U] = static_cast<std::uint8_t>(active_zones >> 8U);
        encoded.parameters[2U] = static_cast<std::uint8_t>(strength_level - 1U);
        break;
    }
    case output::trigger_mode::vibration: {
        if (!std::isfinite(trigger.frequency) || trigger.frequency < 1.0F ||
            trigger.frequency > output::maximum_trigger_frequency ||
            !encode_zonal_effect(trigger, encoded.parameters)) {
            break;
        }

        encoded.mode = trigger_mode_vibration;
        encoded.parameters[8U] = static_cast<std::uint8_t>(std::lround(trigger.frequency));
        break;
    }
    default:
        break;
    }

    return encoded;
}

void write_trigger(feedback_payload& payload,
    const output::trigger_value& trigger,
    std::size_t mode_offset) noexcept {
    const trigger_encoding encoded = encode_trigger(trigger);
    payload[mode_offset] = as_byte(encoded.mode);
    for (std::size_t index = 0U; index < encoded.parameters.size(); ++index) {
        payload[mode_offset + 1U + index] = as_byte(encoded.parameters[index]);
    }
}

[[nodiscard]] std::uint8_t player_led_pattern(std::uint8_t player_index) noexcept {
    switch (player_index) {
    case 0U:
        return 0x00U;
    case 1U:
        return 0x04U;
    case 2U:
        return 0x0AU;
    case 3U:
        return 0x15U;
    case 4U:
        return 0x1BU;
    default:
        return 0x00U;
    }
}

} // namespace

feedback_payload encode_feedback_payload(const output::channel_values& values,
    const output::body_actuator_route route) noexcept {
    const bool hd_mode = route == output::body_actuator_route::hd;
    feedback_payload payload{};
    payload[flags0_offset] = as_byte(hd_mode ? hd_flags0 : classic_flags0);
    payload[flags1_offset] = as_byte(flags1);
    payload[right_motor_offset] =
        as_byte(hd_mode ? 0U : quantize_rumble(values.rumble.high_frequency));
    payload[left_motor_offset] =
        as_byte(hd_mode ? 0U : quantize_rumble(values.rumble.low_frequency));
    payload[microphone_led_offset] = as_byte(values.leds.microphone_led ? 1U : 0U);

    write_trigger(payload, values.right_trigger, right_trigger_mode_offset);
    write_trigger(payload, values.left_trigger, left_trigger_mode_offset);

    payload[player_leds_offset] = as_byte(player_led_pattern(values.leds.player_leds));
    payload[lightbar_red_offset] = as_byte(values.leds.rgb[0U]);
    payload[lightbar_green_offset] = as_byte(values.leds.rgb[1U]);
    payload[lightbar_blue_offset] = as_byte(values.leds.rgb[2U]);
    return payload;
}

} // namespace inputnext::gameinput_detail::dualsense
