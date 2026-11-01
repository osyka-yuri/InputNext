#include "projection.hpp"

#include "effects/effects.hpp"
#include "output/policy/policy.hpp"
#include "projection_support.hpp"

#include <input_next_effects.h>
#include <inputnext/output.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace inputnext::runtime_detail::effects_projection {
namespace {

using effects_projection_support::parse_channels;
using effects_projection_support::valid_fraction;
using effects_projection_support::valid_ttl;
using effects_projection_support::write_channels;

[[nodiscard]] constexpr bool valid_channel_mask(const std::uint32_t value) noexcept {
    return value != 0U && (value & ~INPUT_NEXT_EFFECTS_ALL_CHANNELS) == 0U;
}

[[nodiscard]] bool parse_trigger(const input_next_effects_trigger_v1& value,
    output::trigger_value& out) noexcept {
    switch (value.mode) {
    case INPUT_NEXT_EFFECTS_TRIGGER_OFF:
        out.mode = output::trigger_mode::off;
        break;
    case INPUT_NEXT_EFFECTS_TRIGGER_FEEDBACK:
        out.mode = output::trigger_mode::feedback;
        break;
    case INPUT_NEXT_EFFECTS_TRIGGER_WEAPON:
        out.mode = output::trigger_mode::weapon;
        break;
    case INPUT_NEXT_EFFECTS_TRIGGER_VIBRATION:
        out.mode = output::trigger_mode::vibration;
        break;
    default:
        return false;
    }
    out.layout = output::trigger_layout::uniform;
    out.start = value.start;
    out.end = value.end;
    out.strength = value.strength;
    out.frequency = value.frequency;
    return std::isfinite(value.start) && std::isfinite(value.end) &&
           std::isfinite(value.strength) && std::isfinite(value.frequency);
}

} // namespace

bool parse_submit_request(const input_next_effects_request_v1& value,
    effects::context_handle& context,
    output::submit_request& out) noexcept {
    context = {};
    out = {};
    if (value.struct_size != sizeof(value) || value.reserved != 0U ||
        !valid_channel_mask(value.channel_mask) || !valid_ttl(value.ttl_ms) ||
        value.mutation_sequence == 0U || !parse_context(value.context, context) ||
        !parse_trigger(value.left_trigger, out.values.left_trigger) ||
        !parse_trigger(value.right_trigger, out.values.right_trigger) ||
        !valid_fraction(value.rumble.low_frequency) ||
        !valid_fraction(value.rumble.high_frequency) || value.leds.reserved != 0U ||
        value.leds.lightbar_rgb > 0x00FFFFFFU ||
        value.leds.player_index > output::maximum_player_leds || value.leds.microphone_led > 1U) {
        return false;
    }

    out.selected_channels = parse_channels(value.channel_mask);
    out.sequence = value.mutation_sequence;
    out.ttl = std::chrono::milliseconds{value.ttl_ms};
    out.values.rumble = {.low_frequency = value.rumble.low_frequency,
        .high_frequency = value.rumble.high_frequency};
    out.values.leds = {.rgb = {static_cast<std::uint8_t>((value.leds.lightbar_rgb >> 16U) & 0xFFU),
                           static_cast<std::uint8_t>((value.leds.lightbar_rgb >> 8U) & 0xFFU),
                           static_cast<std::uint8_t>(value.leds.lightbar_rgb & 0xFFU)},
        .player_leds = static_cast<std::uint8_t>(value.leds.player_index),
        .microphone_led = value.leds.microphone_led != 0U};
    return output::policy_detail::validate_submit_request(out).has_value();
}

bool parse_clear_request(const input_next_effects_clear_v1& value,
    effects::context_handle& context,
    output::clear_request& out) noexcept {
    context = {};
    out = {};
    if (value.struct_size != sizeof(value) || !valid_channel_mask(value.channel_mask) ||
        !parse_context(value.context, context)) {
        return false;
    }
    out.selected_channels = parse_channels(value.channel_mask);
    out.sequence = value.mutation_sequence;
    std::ranges::copy(value.channel_revisions, out.expected_revisions.begin());
    return output::policy_detail::validate_clear_request(out).has_value();
}

bool write_discrete_receipt(const effects::discrete_receipt& value,
    input_next_effects_receipt_v1& out) noexcept {
    const auto channels = write_channels(value.accepted_mask);
    if (value.context.id == 0U || !value.context.runtime.valid() || channels == 0U ||
        (value.accepted_mask & ~output::supported_channel_mask) != 0U || value.sequence == 0U ||
        value.publication.value == 0U) {
        return false;
    }
    out = {};
    out.struct_size = sizeof(out);
    out.accepted_mask = channels;
    write_context(value.context, out.context);
    out.mutation_sequence = value.sequence;
    out.publication_revision = value.publication.value;
    std::ranges::copy(value.channel_revisions, out.channel_revisions);
    return true;
}

} // namespace inputnext::runtime_detail::effects_projection
