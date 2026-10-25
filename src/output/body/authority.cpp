#include "output/authority/state.hpp"

#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>

#include "authority_helpers.hpp"
#include "output/policy/body.hpp"
#include "output/policy/policy.hpp"
#include "pcm_collection.hpp"

#include <algorithm>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>

namespace inputnext::output {
result<body_receipt> authority::submit_body(const public_context context,
    const body_request& request) {
    auto submitted = submit_body(context, request, body_playback_update{});
    if (!submitted.has_value()) {
        return std::unexpected{submitted.error()};
    }
    return submitted->body;
}

result<void> authority::implementation::validate_body_lease_locked(const record& entry,
    const body_request& request) const {
    const auto binding = detail::validate_audio_identity(entry.identity, request.audio);
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

result<void> authority::clear_body(const public_context context,
    const body_clear_request& request) {
    const detail::body_pcm_collection_on_exit collect{*this};
    auto& state = *implementation_;
    if (!state.valid_context_identity(context.identity, owner_class::public_game)) {
        return std::unexpected{error::stale_context};
    }
    const auto valid = detail::body_policy::validate_clear_request(request);
    if (!valid.has_value()) {
        return std::unexpected{valid.error()};
    }

    const auto audio_identity = detail::validate_audio_identity(context.identity, request.audio);
    if (!audio_identity.has_value()) {
        return std::unexpected{audio_identity.error()};
    }

    const auto entry_time = state.now();
    std::scoped_lock lock{state.gate};
    const auto evaluation_time = state.effective_time(entry_time);
    static_cast<void>(state.expire_target(context.identity.target, evaluation_time));
    auto found = detail::find_public_body_context(state, context.identity);
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

    state.retire_playback_locked(entry);
    entry.body.reset();
    ++state.logical_version;
    return {};
}

} // namespace inputnext::output
