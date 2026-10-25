#include "represented.hpp"

#include "output/policy/body.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <expected>
#include <optional>

namespace inputnext::output::detail {
namespace {

[[nodiscard]] bool has_diagnostic_rumble(
    const std::optional<channel_snapshot<rumble_value>>& rumble) noexcept {
    return rumble.has_value() && rumble->provenance.priority == internal_priority::diagnostic;
}

} // namespace

execution_result<represented_projection> project_representation(const logical_snapshot& snapshot,
    const channel_mask channels,
    const body_route_support route_support) noexcept {
    if (!snapshot.target.valid() || !snapshot.incarnation.valid() || channels == 0U ||
        (channels & ~supported_channel_mask) != 0U ||
        !body_policy::valid_route_support(route_support)) {
        return std::unexpected{execution_error::invalid_argument};
    }

    represented_projection projection{.target = snapshot.target,
        .incarnation = snapshot.incarnation,
        .channels = channels,
        .route_support = route_support};

    if ((channels & left_trigger_mask) != 0U) {
        projection.left_trigger = snapshot.left_trigger;
    }
    if ((channels & right_trigger_mask) != 0U) {
        projection.right_trigger = snapshot.right_trigger;
    }
    if ((channels & rumble_mask) != 0U) {
        const bool diagnostic_winner = has_diagnostic_rumble(snapshot.rumble);
        if (snapshot.body.has_value()) {
            projection.body = represented_body{
                .intent = body_policy::represented_route(*snapshot.body,
                    route_support,
                    diagnostic_winner),
                .rumble = diagnostic_winner
                              ? rumble_value{}
                              : body_policy::represented_rumble(*snapshot.body, route_support),
            };
        }
        if (!projection.body.has_value() || diagnostic_winner) {
            projection.rumble = snapshot.rumble;
        }
    }
    if ((channels & leds_mask) != 0U) {
        projection.leds = snapshot.leds;
    }

    return projection;
}

std::optional<body_route_intent> represented_body_route(
    const represented_projection& projection) noexcept {
    if (!projection.body.has_value()) {
        return std::nullopt;
    }
    return projection.body->intent;
}

native_output native_values(const represented_projection& projection) noexcept {
    native_output output{.channels = projection.channels};
    if (projection.left_trigger.has_value()) {
        output.values.left_trigger = projection.left_trigger->value;
    }
    if (projection.right_trigger.has_value()) {
        output.values.right_trigger = projection.right_trigger->value;
    }
    const bool diagnostic_winner = has_diagnostic_rumble(projection.rumble);
    if (projection.body.has_value() && !diagnostic_winner) {
        output.values.rumble = projection.body->rumble;
    } else if (projection.rumble.has_value()) {
        output.values.rumble = projection.rumble->value;
    }
    output.body = represented_body_route(projection);
    if (projection.leds.has_value()) {
        output.values.leds = projection.leds->value;
    }
    return output;
}

} // namespace inputnext::output::detail
