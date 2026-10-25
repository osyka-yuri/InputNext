#include "output/authority/state.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>

#include "output/policy/body.hpp"
#include "output/policy/policy.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>

#include <cstdint>

namespace inputnext::output {
namespace {

[[nodiscard]] body_receipt make_receipt(const body_snapshot& body,
    const std::uint64_t version) noexcept {
    return body_receipt{
        .identity = body.intent.identity,
        .accepted_sequence = body.accepted_sequence,
        .logical_version = version,
        .intent = body.intent,
    };
}

[[nodiscard]] result<void> validate_audio_identity(const context_identity context,
    const audio_instance audio) noexcept {
    if (audio.unbound()) {
        return {};
    }
    if (!audio.valid() || audio.target != context.target ||
        audio.authority != context.incarnation) {
        return std::unexpected{error::stale_audio};
    }
    return {};
}

template <typename State>
[[nodiscard]] auto find_public_context(State& state, const context_identity identity) noexcept {
    using record_pointer = decltype(state.records.data());
    if (!state.valid_context_identity(identity, owner_class::public_game)) {
        return result<record_pointer>{std::unexpected{error::stale_context}};
    }
    auto* const entry = state.find_open(identity);
    if (entry == nullptr) {
        return result<record_pointer>{std::unexpected{
            state.find_retained(identity) != nullptr ? error::revoked : error::stale_context}};
    }
    return result<record_pointer>{entry};
}

} // namespace

result<body_receipt> authority::submit_body(const public_context context,
    const body_request& request) {
    auto& state = *implementation_;
    if (!state.valid_context_identity(context.identity, owner_class::public_game)) {
        return std::unexpected{error::stale_context};
    }
    const auto valid = detail::body_policy::validate_request(request);
    if (!valid.has_value()) {
        return std::unexpected{valid.error()};
    }

    const auto audio_identity = validate_audio_identity(context.identity, request.audio);
    if (!audio_identity.has_value()) {
        return std::unexpected{audio_identity.error()};
    }

    const auto entry_time = state.now();
    const auto deadline =
        policy_detail::make_deadline(entry_time, request.ttl, ttl_permission::finite_only);
    if (!deadline.has_value()) {
        return std::unexpected{deadline.error()};
    }
    if (!deadline->has_value()) {
        return std::unexpected{error::deadline_overflow};
    }

    std::scoped_lock lock{state.gate};
    const auto evaluation_time = state.effective_time(entry_time);
    static_cast<void>(state.expire_target(context.identity.target, evaluation_time));
    auto found = find_public_context(state, context.identity);
    if (!found.has_value()) {
        return std::unexpected{found.error()};
    }
    auto& entry = **found;
    const auto qualified =
        state.qualify_mutation(entry, entry.identity.target, entry.opening_epoch);
    if (!qualified.has_value()) {
        return std::unexpected{qualified.error()};
    }
    return state.commit_body_locked(entry,
        request,
        {.entered_at = entry_time, .expires_at = **deadline});
}

result<void> authority::implementation::validate_body_lease_locked(const record& entry,
    const body_request& request) const {
    const auto binding = validate_audio_identity(entry.identity, request.audio);
    if (!binding.has_value()) {
        return std::unexpected{binding.error()};
    }

    if (!request.lease.valid()) {
        if (!request.audio.unbound() && !audio_facts(request.audio).has_value()) {
            return std::unexpected{error::stale_audio};
        }
        if (request.sequence != 1U) {
            return std::unexpected{error::sequence_rejected};
        }
        const bool target_owned = std::ranges::any_of(records, [&](const auto& candidate) {
            return candidate.state == phase::open &&
                   candidate.identity.kind == owner_class::public_game &&
                   candidate.identity.target == entry.identity.target && candidate.body.has_value();
        });
        if (target_owned) {
            return std::unexpected{error::busy};
        }
    } else {
        if (!entry.body.has_value() || entry.body->intent.identity.lease != request.lease) {
            return std::unexpected{error::stale_lease};
        }
        if (entry.body->intent.identity.audio != request.audio) {
            return std::unexpected{error::stale_audio};
        }
        if (request.sequence <= entry.body->accepted_sequence) {
            return std::unexpected{error::sequence_rejected};
        }
    }
    return {};
}

result<body_receipt> authority::implementation::commit_body_locked(record& entry,
    const body_request& request,
    const body_submission_time timing) {
    const auto evaluation_time = effective_time(timing.entered_at);
    static_cast<void>(expire_target(entry.identity.target, evaluation_time));
    if (policy_detail::is_expired(timing.expires_at, evaluation_time)) {
        return std::unexpected{error::expired};
    }
    const auto valid_lease = validate_body_lease_locked(entry, request);
    if (!valid_lease.has_value()) {
        return std::unexpected{valid_lease.error()};
    }

    const body_lease_id lease =
        request.lease.valid() ? request.lease : body_lease_id{next_body_lease_id};
    if (!lease.valid()) {
        return std::unexpected{error::identity_exhausted};
    }
    body_lease_identity identity{
        .context = entry.identity,
        .lease = lease,
        .audio = request.audio,
    };
    const auto audio = request.audio.unbound() ? std::nullopt : audio_facts(request.audio);
    const bool diagnostic_winner =
        diagnostic_rumble_wins(entry.identity.target, evaluation_time, entry.opening_epoch);
    const auto planned =
        detail::body_policy::plan_route(identity, request.preference, audio, diagnostic_winner, 0U);

    const auto previous_reservations =
        entry.body.has_value()
            ? 1U + static_cast<std::size_t>(entry.body->intent.route == body_actuator_route::hd)
            : 0U;
    const auto new_reservations =
        1U + static_cast<std::size_t>(planned.route == body_actuator_route::hd);
    const auto reservations_after = reservation_count() - previous_reservations + new_reservations;
    if (!ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
    }
    if (!request.lease.valid()) {
        const auto allocated = take_body_lease_id();
        if (!allocated.has_value()) {
            return std::unexpected{allocated.error()};
        }
        identity.lease = *allocated;
    }
    const auto version = logical_version + 1U;
    auto intent = planned;
    intent.identity = identity;
    intent.revision = version;
    entry.body = body_snapshot{
        .intent = intent,
        .accepted_sequence = request.sequence,
        .expires_at = timing.expires_at,
        .fallback_rumble = request.fallback,
    };
    logical_version = version;
    return make_receipt(*entry.body, version);
}

result<void> authority::clear_body(const public_context context,
    const body_clear_request& request) {
    auto& state = *implementation_;
    if (!state.valid_context_identity(context.identity, owner_class::public_game)) {
        return std::unexpected{error::stale_context};
    }
    const auto valid = detail::body_policy::validate_clear_request(request);
    if (!valid.has_value()) {
        return std::unexpected{valid.error()};
    }

    const auto audio_identity = validate_audio_identity(context.identity, request.audio);
    if (!audio_identity.has_value()) {
        return std::unexpected{audio_identity.error()};
    }

    const auto entry_time = state.now();
    std::scoped_lock lock{state.gate};
    const auto evaluation_time = state.effective_time(entry_time);
    static_cast<void>(state.expire_target(context.identity.target, evaluation_time));
    auto found = find_public_context(state, context.identity);
    if (!found.has_value()) {
        return std::unexpected{found.error()};
    }
    auto& entry = **found;
    const auto qualified =
        state.qualify_mutation(entry, entry.identity.target, entry.opening_epoch);
    if (!qualified.has_value()) {
        return std::unexpected{qualified.error()};
    }
    static_cast<void>(state.expire_target(entry.identity.target, state.effective_time(entry_time)));
    if (!entry.body.has_value() || entry.body->intent.identity.lease != request.lease) {
        return std::unexpected{error::stale_lease};
    }
    if (entry.body->intent.identity.audio != request.audio) {
        return std::unexpected{error::stale_audio};
    }
    if (request.sequence <= entry.body->accepted_sequence) {
        return std::unexpected{error::sequence_rejected};
    }

    const auto reservations_after =
        state.reservation_count() - 1U -
        static_cast<std::size_t>(entry.body->intent.route == body_actuator_route::hd);
    if (!state.ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
    }

    entry.body.reset();
    ++state.logical_version;
    return {};
}

} // namespace inputnext::output
