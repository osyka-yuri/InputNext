#pragma once

#include <input_next_effects.h>
#include <inputnext/output.hpp>

#include <cmath>
#include <cstdint>

namespace inputnext::runtime_detail::effects_projection_support {

[[nodiscard]] inline bool valid_fraction(const float value) noexcept {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] inline bool valid_ttl(const std::uint32_t value) noexcept {
    return value != 0U && value <= INPUT_NEXT_EFFECTS_MAX_TTL_MS;
}

[[nodiscard]] inline output::channel_mask parse_channels(const std::uint32_t value) noexcept {
    output::channel_mask result{};
    if ((value & INPUT_NEXT_EFFECTS_LEFT_TRIGGER) != 0U) {
        result |= output::left_trigger_mask;
    }
    if ((value & INPUT_NEXT_EFFECTS_RIGHT_TRIGGER) != 0U) {
        result |= output::right_trigger_mask;
    }
    if ((value & INPUT_NEXT_EFFECTS_RUMBLE) != 0U) {
        result |= output::rumble_mask;
    }
    if ((value & INPUT_NEXT_EFFECTS_LEDS) != 0U) {
        result |= output::leds_mask;
    }
    return result;
}

[[nodiscard]] inline std::uint32_t write_channels(const output::channel_mask value) noexcept {
    std::uint32_t result{};
    if ((value & output::left_trigger_mask) != 0U) {
        result |= INPUT_NEXT_EFFECTS_LEFT_TRIGGER;
    }
    if ((value & output::right_trigger_mask) != 0U) {
        result |= INPUT_NEXT_EFFECTS_RIGHT_TRIGGER;
    }
    if ((value & output::rumble_mask) != 0U) {
        result |= INPUT_NEXT_EFFECTS_RUMBLE;
    }
    if ((value & output::leds_mask) != 0U) {
        result |= INPUT_NEXT_EFFECTS_LEDS;
    }
    return result;
}

} // namespace inputnext::runtime_detail::effects_projection_support
