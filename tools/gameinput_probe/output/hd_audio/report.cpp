#include "gameinput_probe/output/hd_audio/report.hpp"

#include "gameinput_probe/output/dualsense_usb/report.hpp"
#include "gameinput_probe/output/hd_audio/evidence.hpp"
#include "gameinput_probe/output/rumble/report.hpp"

#include "audio/audio_service.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <iomanip>
#include <ios>
#include <optional>
#include <ostream>
#include <string_view>

namespace inputnext::gameinput_detail {
namespace {

[[nodiscard]] std::string_view phase_name(const audio_detail::audio_target_phase phase) noexcept {
    switch (phase) {
    case audio_detail::audio_target_phase::waiting_for_backend:
        return "waiting_for_backend";
    case audio_detail::audio_target_phase::waiting_for_retry:
        return "waiting_for_retry";
    case audio_detail::audio_target_phase::opening:
        return "opening";
    case audio_detail::audio_target_phase::waiting_for_reservation:
        return "waiting_for_reservation";
    case audio_detail::audio_target_phase::live:
        return "live";
    case audio_detail::audio_target_phase::revoking:
        return "revoking";
    case audio_detail::audio_target_phase::draining:
        return "draining";
    case audio_detail::audio_target_phase::retained:
        return "retained";
    }
    return "unknown";
}

[[nodiscard]] std::string_view route_name(const output::body_actuator_route route) noexcept {
    switch (route) {
    case output::body_actuator_route::classic:
        return "classic";
    case output::body_actuator_route::hd:
        return "hd";
    }
    return "unknown";
}

void append_audio_identity(std::ostream& output, const output::audio_instance& audio) {
    output << " audio_target=" << format_rumble_target(audio.target) << " audio_authority=";
    for (const auto byte : audio.authority.bytes) {
        output << std::hex << std::setw(2) << std::setfill('0')
               << std::to_integer<unsigned int>(byte);
    }
    output << std::dec << " audio_id=" << audio.id.value;
}

} // namespace

void write_hd_audio_progress(std::ostream& output,
    const connection_target target,
    const std::optional<audio_detail::audio_target_progress>& audio,
    const std::optional<output::backend_progress>& backend) {
    output << "hd_audio_progress " << format_rumble_target(target);
    if (backend.has_value()) {
        output << " backend=" << backend->backend.incarnation.id.value
               << " backend_phase=" << static_cast<unsigned int>(backend->phase);
        if (backend->acknowledged_body_route.has_value()) {
            const auto& route = *backend->acknowledged_body_route;
            output << " route_ack=true route=" << route_name(route.intent.route)
                   << " route_revision=" << route.intent.revision
                   << " route_publication=" << route.publication.value
                   << " route_attempt=" << route.attempt.value;
        } else {
            output << " route_ack=false";
        }
    } else {
        output << " backend=unavailable route_ack=false";
    }
    if (!audio.has_value()) {
        output << " audio_phase=unavailable accepted_pcm_buffers=0 accepted_pcm_frames=0"
                  " accepted_silent_buffers=0 admitted_blocks=0 source_frames_consumed=0\n";
        return;
    }
    const auto& state = *audio;
    output << " audio_phase=" << phase_name(state.phase) << " audio_ready=" << std::boolalpha
           << state.ready << " stream_started=" << state.native_stream_started
           << " transport_open=" << state.transport_open;
    if (state.audio.has_value()) {
        append_audio_identity(output, *state.audio);
    }
    if (state.last_admitted_route.has_value()) {
        const auto& route = *state.last_admitted_route;
        output << " admitted_route_backend=" << route.backend.incarnation.id.value
               << " admitted_route_publication=" << route.publication.value
               << " admitted_route_attempt=" << route.attempt.value
               << " admitted_route_revision=" << route.intent.revision;
    } else {
        output << " admitted_route=none";
    }
    if (state.last_admitted_playback.has_value()) {
        const auto& playback = *state.last_admitted_playback;
        output << " admitted_context=" << playback.lease.context.id.value
               << " admitted_lease=" << playback.lease.lease.value
               << " admitted_clip_revision=" << playback.clip_revision;
    } else {
        output << " admitted_clip=none";
    }
    output << " admitted_blocks=" << state.admitted_blocks
           << " source_frames_consumed=" << state.source_frames_consumed
           << " accepted_pcm_buffers=" << state.accepted_pcm_buffers
           << " accepted_pcm_frames=" << state.accepted_pcm_frames
           << " accepted_silent_buffers=" << state.accepted_silent_buffers
           << " native_operation=" << static_cast<unsigned int>(state.last_native_operation);
    if (state.last_native_error.has_value()) {
        output << " native_error_operation="
               << static_cast<unsigned int>(state.last_native_error->operation)
               << " native_error_code=0x" << std::hex << state.last_native_error->native_code
               << std::dec;
    }
    output << '\n';
}

void write_hd_audio_step(std::ostream& output,
    const std::string_view name,
    const connection_target target,
    const std::size_t step_number,
    const std::size_t step_count,
    const output::context_identity context,
    const output::body_playback_receipt& receipt,
    const hd_usb_evidence usb_evidence,
    const std::optional<hd_usb_match>& match,
    const bool pcm_accepted,
    const bool fallback_observed) {
    output << "hd_audio_step name=" << name << " target=" << format_rumble_target(target)
           << " number=" << step_number << '/' << step_count << " context=" << context.id.value
           << " lease=" << receipt.body.identity.lease.value
           << " accepted_sequence=" << receipt.body.accepted_sequence
           << " logical_version=" << receipt.body.logical_version
           << " route=" << route_name(receipt.body.intent.route)
           << " route_revision=" << receipt.body.intent.revision
           << " usb_evidence=" << hd_usb_evidence_name(usb_evidence)
           << " pcm_accepted=" << std::boolalpha << pcm_accepted
           << " fallback_observed=" << fallback_observed;
    if (receipt.playback.has_value()) {
        output << " clip_revision=" << receipt.playback->clip_revision;
    } else {
        output << " clip_revision=none";
    }
    if (match.has_value()) {
        output << " backend=" << match->backend.incarnation.id.value
               << " publication=" << match->publication.value << " attempt=" << match->attempt.value
               << " native_status=" << dualsense_usb_status_name(match->result.status)
               << " native_code=" << match->result.native_code;
    } else {
        output << " backend=unconfirmed native_status=unconfirmed";
    }
    output << '\n';
}

} // namespace inputnext::gameinput_detail
