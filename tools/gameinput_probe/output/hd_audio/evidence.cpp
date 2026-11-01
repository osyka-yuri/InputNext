#include "gameinput_probe/output/hd_audio/evidence.hpp"

#include "audio/audio_service.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <optional>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] bool is_historical_reuse(const hd_usb_match& candidate,
    const output::backend_progress& progress,
    const std::optional<hd_usb_match>& previous_match) noexcept {
    return previous_match.has_value() && *previous_match == candidate &&
           progress.last_attempt == candidate.attempt && !progress.write_in_flight &&
           progress.delivery == output::delivery_state::setter_accepted &&
           candidate.result.status == output::native_status::accepted;
}

} // namespace

hd_usb_evidence assess_hd_usb_evidence(const output::backend_handle& expected_backend,
    const output::body_snapshot& expected_body,
    const output::native_output& expected_output,
    const std::optional<output::attempt_id> before_attempt,
    const output::backend_progress& progress,
    const output::logical_snapshot& logical,
    const std::optional<hd_usb_match>& previous_match,
    hd_usb_match& matched) noexcept {
    if (!expected_backend.valid() || !progress.backend.valid()) {
        return hd_usb_evidence::pending;
    }
    if (progress.backend != expected_backend || progress.phase != output::backend_phase::current ||
        progress.represented_channels != output::supported_channel_mask ||
        progress.unsupported_channels != 0U || logical.target != expected_backend.target ||
        logical.incarnation != expected_backend.incarnation.owner.authority) {
        return hd_usb_evidence::rejected;
    }
    if (!logical.body.has_value()) {
        return hd_usb_evidence::pending;
    }
    if (logical.body->intent != expected_body.intent ||
        logical.body->accepted_sequence != expected_body.accepted_sequence ||
        logical.body->fallback_rumble != expected_body.fallback_rumble) {
        return hd_usb_evidence::rejected;
    }
    if (!progress.observed_publication.valid() || !progress.last_completed_attempt.has_value()) {
        return hd_usb_evidence::pending;
    }

    const auto& completed = *progress.last_completed_attempt;
    if (completed.publication == progress.observed_publication &&
        completed.result.status != output::native_status::accepted &&
        completed.result.status != output::native_status::retryable_failure) {
        return hd_usb_evidence::rejected;
    }
    if (completed.output != expected_output ||
        completed.output.channels != output::supported_channel_mask ||
        !progress.last_attempt.has_value() || progress.last_attempt != completed.id ||
        progress.write_in_flight || progress.delivery != output::delivery_state::setter_accepted ||
        completed.result.status != output::native_status::accepted) {
        return hd_usb_evidence::pending;
    }

    if (expected_body.intent.route == output::body_actuator_route::hd) {
        if (!progress.acknowledged_body_route.has_value() ||
            progress.acknowledged_body_route->backend != expected_backend ||
            progress.acknowledged_body_route->intent != expected_body.intent ||
            progress.acknowledged_body_route->publication != completed.publication ||
            progress.acknowledged_body_route->attempt != completed.id) {
            return hd_usb_evidence::pending;
        }
    } else if (progress.acknowledged_body_route.has_value() &&
               progress.acknowledged_body_route->intent.identity == expected_body.intent.identity) {
        return hd_usb_evidence::pending;
    }

    const hd_usb_match candidate{.backend = progress.backend,
        .intent = expected_body.intent,
        .native_output = completed.output,
        .publication = completed.publication,
        .attempt = completed.id,
        .result = completed.result};
    if (completed.publication == progress.observed_publication) {
        const bool newer =
            !before_attempt.has_value() || completed.id.value > before_attempt->value;
        if (newer) {
            matched = candidate;
            return hd_usb_evidence::accepted;
        }
    }

    if (is_historical_reuse(candidate, progress, previous_match)) {
        matched = candidate;
        return hd_usb_evidence::accepted;
    }
    return hd_usb_evidence::pending;
}

bool accepted_hd_pcm_progress(const output::backend_handle& backend,
    const output::audio_instance& audio,
    const output::body_route_ack& route,
    const output::body_playback_identity& playback,
    const audio_detail::audio_target_progress& progress,
    const std::uint64_t previous_buffers,
    const std::uint64_t previous_frames,
    const std::uint64_t previous_source_frames,
    const std::uint64_t required_source_frames) noexcept {
    return progress.target == backend.target && progress.backend == backend &&
           progress.audio == audio && progress.phase == audio_detail::audio_target_phase::live &&
           progress.transport_open && progress.native_stream_started && progress.ready &&
           progress.last_admitted_route == route && progress.last_admitted_playback == playback &&
           !progress.counters_saturated && progress.accepted_pcm_frames >= previous_frames &&
           progress.source_frames_consumed >= previous_source_frames &&
           progress.accepted_pcm_buffers > previous_buffers &&
           progress.accepted_pcm_frames - previous_frames >= required_source_frames &&
           progress.source_frames_consumed - previous_source_frames >= required_source_frames;
}

const char* hd_usb_evidence_name(const hd_usb_evidence value) noexcept {
    switch (value) {
    case hd_usb_evidence::pending:
        return "pending";
    case hd_usb_evidence::accepted:
        return "accepted_native_setter";
    case hd_usb_evidence::rejected:
        return "rejected";
    }
    return "unknown";
}

} // namespace inputnext::gameinput_detail
