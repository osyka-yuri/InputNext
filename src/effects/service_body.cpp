#include "implementation.hpp"

#include "audio/audio_service.hpp"
#include "backend_observation.hpp"
#include "error_mapping.hpp"
#include "identity.hpp"

#include "effects/effects.hpp"

#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_submission.hpp>

#include <chrono>
#include <cstdint>
#include <expected>
#include <mutex>
#include <new>
#include <optional>

namespace inputnext::effects {
namespace {

[[nodiscard]] status validate_body_command(const body_request& request) noexcept {
    switch (request.pcm_command) {
    case output::body_pcm_command::keep:
    case output::body_pcm_command::stop:
        return request.replacement.has_value() ? status::invalid_argument : status::ok;
    case output::body_pcm_command::replace:
        return request.replacement.has_value() ? status::ok : status::invalid_argument;
    }
    return status::invalid_argument;
}

[[nodiscard]] status resolve_new_audio(const service_dependencies& dependencies,
    const connection_target target,
    const std::uint64_t audio_id,
    const gameinput_detail::managed_backend_observation& backend,
    output::audio_instance& out) {
    out = {};
    if (audio_id == 0U) {
        return status::ok;
    }
    const auto progress_result = dependencies.audio.progress(target);
    if (!progress_result.has_value()) {
        return status::stale_lease;
    }
    const auto progress = progress_result.value();
    if (!progress.audio.has_value()) {
        return status::stale_lease;
    }
    const auto instance = progress.audio.value();
    if (instance.id.value != audio_id || instance.target != target) {
        return status::stale_lease;
    }
    if (!progress.backend.has_value() ||
        progress.backend.value() != backend.execution.progress.backend) {
        return status::busy;
    }
    try {
        const auto observed = dependencies.authority.inspect_audio(instance);
        if (!observed.has_value()) {
            return map_output_error(observed.error());
        }
        if (observed->instance != instance) {
            return status::stale_lease;
        }
        out = instance;
        return status::ok;
    } catch (const std::bad_alloc&) {
        return status::capacity_exceeded;
    } catch (...) {
        return status::io_failed;
    }
}

struct body_context_snapshot final {
    connection_target target{};
    output::public_context native_context{};
    std::optional<output::body_lease_identity> body_identity;
};

template <typename State>
[[nodiscard]] status
capture_body_context(State& state, const context_handle context, body_context_snapshot& out) {
    std::scoped_lock lock{state.gate};
    if (!state.accepting) {
        return status::unavailable;
    }
    const auto found = state.find_open(context);
    if (!found.has_value()) {
        return found.error();
    }
    const auto* record = found.value();
    out = body_context_snapshot{.target = record->target,
        .native_context = record->native_context,
        .body_identity = record->body_identity};
    return status::ok;
}

[[nodiscard]] status resolve_body_lease(const service_dependencies& dependencies,
    const body_request& request,
    const body_context_snapshot& snapshot,
    const gameinput_detail::managed_backend_observation& backend,
    output::body_lease_id& lease,
    output::audio_instance& audio) {
    if (request.lease_id == 0U) {
        return resolve_new_audio(dependencies,
            snapshot.target,
            request.audio_instance_id,
            backend,
            audio);
    }
    if (!snapshot.body_identity.has_value()) {
        return status::stale_lease;
    }
    const auto& identity = snapshot.body_identity.value();
    if (identity.lease.value != request.lease_id ||
        identity.audio.id.value != request.audio_instance_id ||
        identity.context != snapshot.native_context.identity) {
        return status::stale_lease;
    }
    lease = identity.lease;
    audio = identity.audio;
    return status::ok;
}

[[nodiscard]] status prepare_body_samples(output::authority& authority,
    const body_request& request,
    std::optional<output::prepared_body_pcm>& out) {
    if (request.pcm_command != output::body_pcm_command::replace) {
        return status::ok;
    }
    if (!request.replacement.has_value()) {
        return status::invalid_argument;
    }
    try {
        const auto owner = authority.pcm_preparer();
        const auto prepared = owner.prepare(request.replacement.value());
        if (!prepared.has_value()) {
            return map_output_error(prepared.error());
        }
        out = prepared.value();
        return status::ok;
    } catch (const std::bad_alloc&) {
        return status::capacity_exceeded;
    } catch (...) {
        return status::io_failed;
    }
}

template <typename State>
[[nodiscard]] status validate_body_commit(State& state,
    const body_request& request,
    const body_context_snapshot& snapshot,
    context_record*& out) {
    if (!state.accepting) {
        return status::unavailable;
    }
    const auto found = state.find_open(request.context);
    if (!found.has_value()) {
        return found.error();
    }
    out = found.value();
    if (out->native_context.identity != snapshot.native_context.identity) {
        return status::stale_context;
    }
    if (request.lease_id == 0U) {
        return status::ok;
    }
    if (!snapshot.body_identity.has_value() || !out->body_identity.has_value()) {
        return status::stale_lease;
    }
    const auto& captured = snapshot.body_identity.value();
    const auto& current = out->body_identity.value();
    if (current.lease != captured.lease || current.audio != captured.audio) {
        return status::stale_lease;
    }
    return status::ok;
}

template <typename State>
[[nodiscard]] result<body_receipt> commit_body(State& state,
    const body_request& request,
    const body_context_snapshot& snapshot,
    const output::body_request& native_request,
    const output::body_playback_update& playback,
    const output::finite_submission_window& window) {
    std::scoped_lock lock{state.gate};
    context_record* record{};
    const auto valid = validate_body_commit(state, request, snapshot, record);
    if (valid != status::ok) {
        return std::unexpected{valid};
    }

    const service_dependencies& dependencies = state.dependencies;
    const auto publication = dependencies.identities.take_publication();
    if (!publication.has_value()) {
        return std::unexpected{publication.error()};
    }

    try {
        const auto accepted = dependencies.authority.submit_body(record->native_context,
            native_request,
            playback,
            window);
        if (!accepted.has_value()) {
            return std::unexpected{map_output_error(accepted.error())};
        }
        if (accepted->body.identity.context != record->native_context.identity ||
            accepted->body.accepted_sequence != request.sequence ||
            !accepted->body.identity.valid() || accepted->body.logical_version == 0U) {
            return std::unexpected{status::io_failed};
        }

        record->body_identity = accepted->body.identity;
        record->latest_body = body_marker{.present = true,
            .publication = publication.value(),
            .receipt = accepted.value()};
        return body_receipt{.context = request.context,
            .target = snapshot.target,
            .lease_id = accepted->body.identity.lease.value,
            .audio_instance_id = accepted->body.identity.audio.id.value,
            .sequence = accepted->body.accepted_sequence,
            .publication = publication.value(),
            .route = accepted->body.intent.route,
            .fallback = accepted->body.intent.fallback};
    } catch (const std::bad_alloc&) {
        return std::unexpected{status::capacity_exceeded};
    } catch (...) {
        return std::unexpected{status::io_failed};
    }
}

} // namespace

result<body_receipt> service::submit_body(const body_request& request,
    const output::finite_submission_window& window) {
    auto& state = *implementation_;
    if (request.ttl <= std::chrono::milliseconds::zero() || request.ttl != window.ttl()) {
        return std::unexpected{status::invalid_argument};
    }
    const auto command_valid = validate_body_command(request);
    if (command_valid != status::ok) {
        return std::unexpected{command_valid};
    }

    body_context_snapshot snapshot{};
    const auto captured = capture_body_context(state, request.context, snapshot);
    if (captured != status::ok) {
        return std::unexpected{captured};
    }

    gameinput_detail::managed_backend_observation backend{};
    const auto supported = current_backend_support(state.dependencies.connections,
        snapshot.target,
        output::rumble_mask,
        backend);
    if (supported != status::ok) {
        return std::unexpected{supported};
    }

    output::body_lease_id lease{};
    output::audio_instance audio{};
    const auto identity_status =
        resolve_body_lease(state.dependencies, request, snapshot, backend, lease, audio);
    if (identity_status != status::ok) {
        return std::unexpected{identity_status};
    }

    std::optional<output::prepared_body_pcm> prepared;
    const auto prepared_status =
        prepare_body_samples(state.dependencies.authority, request, prepared);
    if (prepared_status != status::ok) {
        return std::unexpected{prepared_status};
    }

    const output::body_request native_request{.lease = lease,
        .audio = audio,
        .sequence = request.sequence,
        .ttl = request.ttl,
        .preference = request.preference,
        .fallback = request.fallback};
    const output::body_playback_update playback{.command = request.pcm_command,
        .replacement = prepared.value_or(output::prepared_body_pcm{})};
    return commit_body(state, request, snapshot, native_request, playback, window);
}

status service::clear_body(const body_clear_request& request) {
    auto& state = *implementation_;
    connection_target target{};
    output::public_context native_context{};
    std::optional<output::body_lease_identity> existing_identity;
    {
        std::scoped_lock lock{state.gate};
        if (!state.accepting) {
            return status::unavailable;
        }
        const auto found = state.find_open(request.context);
        if (!found.has_value()) {
            return found.error();
        }
        const auto* record = found.value();
        target = record->target;
        native_context = record->native_context;
        existing_identity = record->body_identity;
    }
    if (request.lease_id == 0U || !existing_identity.has_value()) {
        return status::stale_lease;
    }
    const auto identity = existing_identity.value();
    if (identity.lease.value != request.lease_id ||
        identity.audio.id.value != request.audio_instance_id ||
        identity.context != native_context.identity) {
        return status::stale_lease;
    }

    gameinput_detail::managed_backend_observation backend{};
    const auto supported = current_backend_support(state.dependencies.connections,
        target,
        output::rumble_mask,
        backend);
    if (supported != status::ok) {
        return supported;
    }

    const output::body_clear_request native_request{.lease = identity.lease,
        .audio = identity.audio,
        .sequence = request.sequence};
    std::scoped_lock lock{state.gate};
    if (!state.accepting) {
        return status::unavailable;
    }
    const auto found = state.find_open(request.context);
    if (!found.has_value()) {
        return found.error();
    }
    auto* record = found.value();
    if (record->native_context.identity != native_context.identity ||
        !record->body_identity.has_value()) {
        return status::stale_lease;
    }
    const auto& current_identity = record->body_identity.value();
    if (current_identity.lease != identity.lease || current_identity.audio != identity.audio) {
        return status::stale_lease;
    }

    try {
        const auto cleared =
            state.dependencies.authority.clear_body(record->native_context, native_request);
        if (!cleared.has_value()) {
            return map_output_error(cleared.error());
        }
        record->body_identity.reset();
        return status::ok;
    } catch (const std::bad_alloc&) {
        return status::capacity_exceeded;
    } catch (...) {
        return status::io_failed;
    }
}

} // namespace inputnext::effects
