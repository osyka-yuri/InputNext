#include "represented.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <expected>

#include <inputnext/native_output.hpp>

namespace inputnext::output::detail {

execution_result<represented_projection> project_representation(const logical_snapshot& snapshot,
    const channel_mask channels) noexcept {
    if (!snapshot.target.valid() || !snapshot.incarnation.valid() || channels == 0U ||
        (channels & ~supported_channel_mask) != 0U) {
        return std::unexpected{execution_error::invalid_argument};
    }

    represented_projection projection{.target = snapshot.target,
        .incarnation = snapshot.incarnation,
        .channels = channels};

    if ((channels & left_trigger_mask) != 0U) {
        projection.left_trigger = snapshot.left_trigger;
    }
    if ((channels & right_trigger_mask) != 0U) {
        projection.right_trigger = snapshot.right_trigger;
    }
    if ((channels & rumble_mask) != 0U) {
        projection.rumble = snapshot.rumble;
    }
    if ((channels & leds_mask) != 0U) {
        projection.leds = snapshot.leds;
    }

    return projection;
}

native_output native_values(const represented_projection& projection) noexcept {
    native_output output{.channels = projection.channels};
    if (projection.left_trigger.has_value()) {
        output.values.left_trigger = projection.left_trigger->value;
    }
    if (projection.right_trigger.has_value()) {
        output.values.right_trigger = projection.right_trigger->value;
    }
    if (projection.rumble.has_value()) {
        output.values.rumble = projection.rumble->value;
    }
    if (projection.leds.has_value()) {
        output.values.leds = projection.leds->value;
    }
    return output;
}

} // namespace inputnext::output::detail
