#include "rumble_evidence.hpp"

#include "rumble_sequence.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>
#include <inputnext/xinput.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {

rumble_native_evidence assess_native_evidence(const rumble_step& step,
    const output::backend_handle& expected_backend,
    const std::optional<output::attempt_id> before_attempt,
    const rumble_native_observation& observation,
    const std::optional<rumble_native_match>& previous_match,
    rumble_native_match& matched) noexcept {
    if (!step.expects_native_output) {
        return rumble_native_evidence::not_requested;
    }
    if (!expected_backend.valid() || !observation.progress.backend.valid()) {
        return rumble_native_evidence::pending;
    }
    if (observation.progress.backend != expected_backend ||
        observation.progress.phase != output::backend_phase::current ||
        (observation.progress.represented_channels & output::rumble_mask) == 0U) {
        return rumble_native_evidence::rejected;
    }

    const auto expected_value = normalized_value(step);
    if (observation.logical_channel_present != step.logical_channel_present ||
        observation.logical_value != expected_value) {
        return rumble_native_evidence::pending;
    }

    const auto& progress = observation.progress;
    if (!progress.observed_publication.valid() || !progress.last_completed_attempt.has_value()) {
        return rumble_native_evidence::pending;
    }
    const auto& attempt = *progress.last_completed_attempt;
    if (attempt.publication == progress.observed_publication &&
        attempt.result.status != output::native_status::accepted &&
        attempt.result.status != output::native_status::retryable_failure) {
        return rumble_native_evidence::rejected;
    }
    if (attempt.publication != progress.observed_publication ||
        progress.last_attempt != attempt.id || progress.write_in_flight ||
        progress.delivery != output::delivery_state::setter_accepted ||
        attempt.result.status != output::native_status::accepted) {
        return rumble_native_evidence::pending;
    }
    if (attempt.output.channels != output::rumble_mask ||
        attempt.output.values.rumble != expected_value) {
        return rumble_native_evidence::pending;
    }

    const rumble_native_match candidate{.backend = progress.backend,
        .output = attempt.output,
        .publication = attempt.publication,
        .attempt = attempt.id,
        .result = attempt.result};
    const bool newer_attempt =
        !before_attempt.has_value() || attempt.id.value > before_attempt->value;
    if (newer_attempt) {
        matched = candidate;
        return rumble_native_evidence::accepted_newer_attempt;
    }

    if (previous_match.has_value() && previous_match->backend == progress.backend &&
        previous_match->output == attempt.output &&
        previous_match->publication == progress.observed_publication &&
        previous_match->attempt == attempt.id && previous_match->result == attempt.result) {
        matched = candidate;
        return rumble_native_evidence::historical_reuse;
    }
    return rumble_native_evidence::pending;
}

bool assess_logical_evidence(const rumble_step& step,
    const std::optional<xinput::rumble_output_status>& before_status,
    const std::optional<xinput::rumble_output_status>& after_status,
    const std::optional<std::uint32_t> set_state_result,
    const bool projection_available,
    const bool logical_channel_present,
    const output::rumble_value logical_value,
    const std::optional<std::uint64_t> logical_channel_version) noexcept {
    if (!after_status.has_value() || !after_status->configured || !projection_available ||
        logical_channel_present != step.logical_channel_present) {
        return false;
    }
    if (logical_channel_present) {
        if (logical_value != normalized_value(step) || !logical_channel_version.has_value() ||
            *logical_channel_version != after_status->logical_version) {
            return false;
        }
    } else if (after_status->logical_version != 0U) {
        return false;
    }

    switch (step.action) {
    case rumble_step_action::set_state:
    case rumble_step_action::set_disabled_cache:
        return set_state_result.has_value() &&
               *set_state_result == static_cast<std::uint32_t>(ERROR_SUCCESS) &&
               after_status->desired_state_cached && before_status.has_value() &&
               after_status->request_revision > before_status->request_revision;
    case rumble_step_action::disable:
        return after_status->desired_state_cached && before_status.has_value() &&
               after_status->request_revision == before_status->request_revision;
    case rumble_step_action::enable:
        return after_status->desired_state_cached && !after_status->restore_pending &&
               after_status->last_restore_error == ERROR_SUCCESS;
    }
    return false;
}

} // namespace inputnext::gameinput_detail
