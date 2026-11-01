#include "gameinput_probe/output/dualsense_usb/evidence.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <optional>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] bool logical_values_match(const output::logical_snapshot& logical,
    const dualsense_usb_expectation& expected) noexcept {
    if (logical.present_channels() != expected.logical_channels) {
        return false;
    }
    return (!logical.left_trigger.has_value() ||
               logical.left_trigger->value == expected.logical_values.left_trigger) &&
           (!logical.right_trigger.has_value() ||
               logical.right_trigger->value == expected.logical_values.right_trigger) &&
           (!logical.rumble.has_value() ||
               logical.rumble->value == expected.logical_values.rumble) &&
           (!logical.leds.has_value() || logical.leds->value == expected.logical_values.leds);
}

[[nodiscard]] bool is_historical_reuse(const dualsense_usb_match& candidate,
    const output::backend_progress& progress,
    const std::optional<dualsense_usb_match>& previous_match) noexcept {
    return previous_match.has_value() && *previous_match == candidate &&
           progress.last_attempt == candidate.attempt && !progress.write_in_flight &&
           progress.delivery == output::delivery_state::setter_accepted &&
           candidate.result.status == output::native_status::accepted;
}

} // namespace

dualsense_usb_evidence assess_dualsense_usb_evidence(const output::backend_handle& expected_backend,
    const std::optional<output::attempt_id> before_attempt,
    const dualsense_usb_expectation& expected,
    const output::backend_progress& progress,
    const std::optional<output::logical_snapshot>& logical,
    const std::optional<dualsense_usb_match>& previous_match,
    dualsense_usb_match& matched) noexcept {
    if (!expected_backend.valid() || !progress.backend.valid()) {
        return dualsense_usb_evidence::pending;
    }
    if (progress.backend != expected_backend || progress.phase != output::backend_phase::current ||
        progress.represented_channels != output::supported_channel_mask ||
        progress.unsupported_channels != 0U) {
        return dualsense_usb_evidence::rejected;
    }
    if (!logical.has_value()) {
        return dualsense_usb_evidence::pending;
    }
    if (logical->target != expected_backend.target ||
        logical->incarnation != expected_backend.incarnation.owner.authority) {
        return dualsense_usb_evidence::rejected;
    }
    if (!logical_values_match(*logical, expected)) {
        return dualsense_usb_evidence::pending;
    }
    if (!progress.observed_publication.valid() || !progress.last_completed_attempt.has_value()) {
        return dualsense_usb_evidence::pending;
    }

    const auto& completed = *progress.last_completed_attempt;
    if (completed.publication == progress.observed_publication &&
        completed.result.status != output::native_status::accepted &&
        completed.result.status != output::native_status::retryable_failure) {
        return dualsense_usb_evidence::rejected;
    }
    if (completed.output != expected.native_output ||
        completed.output.channels != output::supported_channel_mask ||
        !progress.last_attempt.has_value() || progress.last_attempt != completed.id ||
        progress.write_in_flight || progress.delivery != output::delivery_state::setter_accepted ||
        completed.result.status != output::native_status::accepted) {
        return dualsense_usb_evidence::pending;
    }

    const dualsense_usb_match candidate{.backend = progress.backend,
        .native_output = completed.output,
        .publication = completed.publication,
        .attempt = completed.id,
        .result = completed.result};
    if (completed.publication == progress.observed_publication) {
        const bool newer =
            !before_attempt.has_value() || completed.id.value > before_attempt->value;
        if (newer) {
            matched = candidate;
            return dualsense_usb_evidence::accepted_newer_attempt;
        }
    }

    if (is_historical_reuse(candidate, progress, previous_match)) {
        matched = candidate;
        return dualsense_usb_evidence::historical_reuse;
    }
    return dualsense_usb_evidence::pending;
}

} // namespace inputnext::gameinput_detail
