#include "gameinput_probe/output/rumble/sequence.hpp"

#include <inputnext/output.hpp>

#include <cstdint>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

constexpr std::uint16_t single_motor_strength = 19660U;
constexpr std::uint16_t combined_motor_strength = 9830U;

} // namespace

rumble_sequence make_rumble_sequence(const rumble_motor_selection selection) noexcept {
    rumble_sequence sequence{};
    if (!selection.any()) {
        return sequence;
    }

    const auto append = [&sequence](const std::string_view name,
                            const rumble_step_action action,
                            const std::uint16_t left_motor,
                            const std::uint16_t right_motor,
                            const std::string_view instruction,
                            const bool expects_native_output,
                            const bool logical_channel_present) {
        sequence.steps_[sequence.size_++] = {.name = name,
            .action = action,
            .left_motor = left_motor,
            .right_motor = right_motor,
            .expects_native_output = expects_native_output,
            .logical_channel_present = logical_channel_present,
            .instruction = instruction};
    };

    if (selection.low_motor) {
        append("low_motor",
            rumble_step_action::set_state,
            single_motor_strength,
            0U,
            "Feel the low-frequency vibration.",
            true,
            true);
    }
    if (selection.high_motor) {
        append("high_motor",
            rumble_step_action::set_state,
            0U,
            single_motor_strength,
            "Feel the high-frequency vibration.",
            true,
            true);
    }
    append("zero",
        rumble_step_action::set_state,
        0U,
        0U,
        "Check that the controller is quiet.",
        true,
        true);
    append("disable",
        rumble_step_action::disable,
        0U,
        0U,
        "Check that rumble has stopped.",
        true,
        false);
    append("disabled_cached_request",
        rumble_step_action::set_disabled_cache,
        selection.low_motor ? combined_motor_strength : 0U,
        selection.high_motor ? combined_motor_strength : 0U,
        "Check that no rumble starts while output is disabled.",
        false,
        false);
    append("reenable_restore",
        rumble_step_action::enable,
        selection.low_motor ? combined_motor_strength : 0U,
        selection.high_motor ? combined_motor_strength : 0U,
        "Feel the restored vibration.",
        true,
        true);
    append("final_zero",
        rumble_step_action::set_state,
        0U,
        0U,
        "Check that rumble has stopped.",
        true,
        true);
    return sequence;
}

output::rumble_value normalized_value(const rumble_step& step) noexcept {
    constexpr float maximum = 65535.0F;
    return {.low_frequency = static_cast<float>(step.left_motor) / maximum,
        .high_frequency = static_cast<float>(step.right_motor) / maximum};
}

} // namespace inputnext::gameinput_detail
