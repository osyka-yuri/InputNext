#include "progress_evaluation.hpp"

#include "effects/effects.hpp"
#include "progress_support.hpp"

#include "gameinput/output/connections/backend_selection.hpp"
#include "gameinput/output/connections/manager.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/output_observation.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

namespace inputnext::effects::detail {

[[nodiscard]] progress progress_response(const connection_target target,
    const publication_revision requested,
    const progress_stage stage,
    const status completion_status,
    const publication_revision current,
    const completion_class completion,
    const std::uint64_t attempt) noexcept {
    return progress{.stage = stage,
        .completion = completion,
        .completion_status = completion_status,
        .target = target,
        .requested = requested,
        .current = current,
        .attempt_serial = attempt};
}

namespace {

template <typename Value>
[[nodiscard]] bool matches_provenance(const output::channel_snapshot<Value>& value,
    const output::public_context context,
    const std::uint64_t revision,
    const std::uint64_t sequence) noexcept {
    return value.provenance.identity == context.identity && value.provenance.revision == revision &&
           value.provenance.accepted_sequence == sequence;
}

[[nodiscard]] bool discrete_is_represented(const progress_marker& marker,
    const output::detail::represented_projection& projected) noexcept {
    const auto mask = marker.selected_channels;
    const auto& receipt = marker.discrete;
    for (std::size_t index = 0U; index < output::channel_count; ++index) {
        const auto channel = static_cast<output::channel>(index);
        if ((mask & output::mask_for(channel)) == 0U) {
            continue;
        }
        const auto revision = receipt.resulting_revisions[index];
        bool matches{};
        switch (channel) {
        case output::channel::left_trigger:
            if (!projected.left_trigger.has_value()) {
                return false;
            }
            matches = matches_provenance(*projected.left_trigger,
                marker.native_context,
                revision,
                receipt.accepted_sequence);
            break;
        case output::channel::right_trigger:
            if (!projected.right_trigger.has_value()) {
                return false;
            }
            matches = matches_provenance(*projected.right_trigger,
                marker.native_context,
                revision,
                receipt.accepted_sequence);
            break;
        case output::channel::rumble:
            if (!projected.rumble.has_value()) {
                return false;
            }
            matches = matches_provenance(*projected.rumble,
                marker.native_context,
                revision,
                receipt.accepted_sequence);
            break;
        case output::channel::leds:
            if (!projected.leds.has_value()) {
                return false;
            }
            matches = matches_provenance(*projected.leds,
                marker.native_context,
                revision,
                receipt.accepted_sequence);
            break;
        }
        if (!matches) {
            return false;
        }
    }
    return mask != 0U;
}

[[nodiscard]] bool body_is_represented(const progress_marker& marker,
    const output::logical_snapshot& snapshot,
    const output::detail::represented_projection& projected) noexcept {
    if (!snapshot.body.has_value()) {
        return false;
    }
    if (!projected.body.has_value()) {
        return false;
    }
    const auto& expected = marker.body.body;
    const auto& current = *snapshot.body;
    const auto& represented = *projected.body;
    return current.accepted_sequence == expected.accepted_sequence &&
           current.intent.identity == expected.identity &&
           current.intent.revision == expected.intent.revision &&
           represented.intent == expected.intent &&
           represented.intent.identity.context == marker.native_context.identity &&
           represented.intent.fallback != output::body_fallback_reason::preempted;
}

[[nodiscard]] bool close_is_represented(const progress_marker& marker,
    const output::detail::represented_projection& projected) noexcept {
    const auto& closing = marker.native_context.identity;
    return (!projected.left_trigger.has_value() ||
               projected.left_trigger->provenance.identity != closing) &&
           (!projected.right_trigger.has_value() ||
               projected.right_trigger->provenance.identity != closing) &&
           (!projected.rumble.has_value() || projected.rumble->provenance.identity != closing) &&
           (!projected.leds.has_value() || projected.leds->provenance.identity != closing) &&
           (!projected.body.has_value() || projected.body->intent.identity.context != closing);
}

[[nodiscard]] bool marker_is_represented(const progress_marker& marker,
    const output::logical_snapshot& snapshot,
    const output::detail::represented_projection& projected) noexcept {
    switch (marker.kind) {
    case marker_kind::discrete:
        return discrete_is_represented(marker, projected);
    case marker_kind::body:
        return body_is_represented(marker, snapshot, projected);
    case marker_kind::close:
        return close_is_represented(marker, projected);
    }
    return false;
}

[[nodiscard]] status unsupported_progress_channel(const progress_marker& marker,
    const output::backend_progress& backend) noexcept {
    if (marker.kind == marker_kind::discrete &&
        (backend.represented_channels & marker.selected_channels) != marker.selected_channels) {
        return status::unsupported_channel;
    }
    if (marker.kind == marker_kind::body &&
        (backend.represented_channels & output::rumble_mask) == 0U) {
        return status::unsupported_channel;
    }
    return status::ok;
}

[[nodiscard]] std::optional<output::completed_attempt> accepted_current_attempt(
    const output::backend_progress& backend) noexcept {
    if (backend.delivery != output::delivery_state::setter_accepted || backend.write_in_flight ||
        !backend.observed_publication.valid()) {
        return std::nullopt;
    }
    if (!backend.last_completed_attempt.has_value()) {
        return std::nullopt;
    }
    const auto completed = *backend.last_completed_attempt;
    if (completed.publication != backend.observed_publication ||
        completed.result.status != output::native_status::accepted) {
        return std::nullopt;
    }
    return completed;
}

} // namespace

result<progress> evaluate_progress_observation(const progress_marker& marker,
    const gameinput_detail::managed_backend_observation& observation) {
    const auto& native = observation.execution;
    const auto unsupported = unsupported_progress_channel(marker, native.progress);
    if (unsupported != status::ok) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::not_deliverable,
            unsupported);
    }

    if (native.progress.delivery == output::delivery_state::identity_exhausted) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::not_deliverable,
            status::capacity_exceeded);
    }
    if (native.observed_logical_version < marker.logical_version) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::dispatch_pending,
            status::pending);
    }

    const auto projected = output::detail::project_representation(native.authority_snapshot,
        native.progress.represented_channels,
        native.progress.body_routes);
    if (!projected.has_value()) {
        return std::unexpected{status::io_failed};
    }
    if (!marker_is_represented(marker, native.authority_snapshot, projected.value())) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::superseded,
            status::stale_effect);
    }
    if (!native.projection_current) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::dispatch_pending,
            status::pending);
    }

    const auto failure = delivery_failure(native.progress);
    if (failure != status::pending) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::not_deliverable,
            failure);
    }

    const auto completed = accepted_current_attempt(native.progress);
    if (!completed.has_value()) {
        return progress_response(marker.target,
            marker.publication,
            progress_stage::dispatch_pending,
            status::pending,
            marker.publication);
    }
    const auto attempt = completed.value();

    const auto completion = observation.kind == gameinput_detail::output_backend_kind::dualsense_usb
                                ? completion_class::raw_output_accepted_current
                                : completion_class::setter_returned_current;
    return progress_response(marker.target,
        marker.publication,
        progress_stage::current_completed,
        status::ok,
        marker.publication,
        completion,
        attempt.id.value);
}

} // namespace inputnext::effects::detail
