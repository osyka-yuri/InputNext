#include "output/authority/state.hpp"

#include "authority_helpers.hpp"
#include "output/policy/body.hpp"
#include "output/policy/policy.hpp"
#include "pcm_collection.hpp"

#include "output/playback/body_playback_bank.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_body_pcm.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_submission.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::output {
namespace {

[[nodiscard]] bool valid_command(const body_pcm_command command) noexcept {
    switch (command) {
    case body_pcm_command::keep:
    case body_pcm_command::stop:
    case body_pcm_command::replace:
        return true;
    }
    return false;
}

} // namespace

result<body_playback_receipt> authority::submit_body(const public_context context,
    const body_request& request,
    const body_playback_update& playback) {
    return submit_body_impl(context, request, playback, nullptr);
}

result<body_playback_receipt> authority::submit_body(const public_context context,
    const body_request& request,
    const body_playback_update& playback,
    const finite_submission_window& window) {
    return submit_body_impl(context, request, playback, &window);
}

result<body_playback_receipt> authority::submit_body_impl(const public_context context,
    const body_request& request,
    const body_playback_update& playback,
    const finite_submission_window* const window) {
    const detail::body_pcm_collection_on_exit collect{*this};
    auto& state = *implementation_;
    if (!state.valid_context_identity(context.identity, owner_class::public_game)) {
        return std::unexpected{error::stale_context};
    }
    if (window != nullptr && request.ttl != window->ttl()) {
        return std::unexpected{error::invalid_argument};
    }
    const auto entry_time = window == nullptr ? state.now() : window->entered_at();
    if (!valid_command(playback.command)) {
        return std::unexpected{error::invalid_argument};
    }

    prepared_body_pcm replacement = playback.replacement;
    if (playback.command == body_pcm_command::replace) {
        if (!replacement.valid() || !state.pcm_preparer.owns(replacement)) {
            return std::unexpected{error::invalid_argument};
        }
    } else if (replacement.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    const auto valid = detail::body_policy::validate_request(request);
    if (!valid.has_value()) {
        return std::unexpected{valid.error()};
    }
    const auto audio_identity = detail::validate_audio_identity(context.identity, request.audio);
    if (!audio_identity.has_value()) {
        return std::unexpected{audio_identity.error()};
    }
    if (playback.command == body_pcm_command::replace) {
        collect_body_pcm();
    }
    std::chrono::steady_clock::time_point expiry;
    if (window != nullptr) {
        expiry = window->expires_at();
    } else {
        const auto deadline =
            policy_detail::make_deadline(entry_time, request.ttl, ttl_permission::finite_only);
        if (!deadline.has_value()) {
            return std::unexpected{deadline.error()};
        }
        if (!deadline->has_value()) {
            return std::unexpected{error::deadline_overflow};
        }
        expiry = **deadline;
    }

    detail::body_playback_retirement_batch displaced;
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
    return state.commit_body_playback_locked(entry,
        request,
        {.entered_at = entry_time, .expires_at = expiry, .has_absolute_window = window != nullptr},
        playback.command,
        std::move(replacement),
        displaced);
}

result<body_playback_receipt> authority::implementation::commit_body_playback_locked(record& entry,
    const body_request& request,
    const body_submission_time timing,
    const body_pcm_command command,
    prepared_body_pcm&& replacement,
    detail::body_playback_retirement_batch& displaced) {
    auto plan = prepare_body_playback_commit_locked(entry, request, timing, command, replacement);
    if (!plan.has_value()) {
        return std::unexpected{plan.error()};
    }

    if (timing.has_absolute_window &&
        policy_detail::is_expired(policy_detail::optional_deadline{timing.expires_at},
            effective_time())) {
        return std::unexpected{error::expired};
    }

    if (command == body_pcm_command::keep && entry.body.has_value() &&
        entry.body->intent.route == body_actuator_route::hd &&
        plan->intent.route == body_actuator_route::classic) {
        mark_started_playback_retired_locked(entry);
    }

    if (command == body_pcm_command::replace) {
        if (!plan->install_selection.has_value()) {
            return std::unexpected{error::capacity_exceeded};
        }
        const bool installed = playback_bank.install(*plan->install_selection,
            plan->previous_playback,
            plan->replacement_identity,
            std::move(replacement),
            displaced);
        if (!installed) {
            return std::unexpected{error::capacity_exceeded};
        }
    } else if (command == body_pcm_command::stop) {
        static_cast<void>(playback_bank.retire_lease(plan->identity));
    }

    if (plan->new_lease) {
        const auto allocated = take_body_lease_id();
        if (!allocated.has_value() || *allocated != plan->identity.lease) {
            std::terminate();
        }
    }

    entry.body = body_snapshot{.intent = plan->intent,
        .accepted_sequence = request.sequence,
        .expires_at = timing.expires_at,
        .fallback_rumble = request.fallback};
    if (command == body_pcm_command::replace) {
        entry.playback = plan->replacement_identity;
        next_body_playback_revision =
            next_body_playback_revision == std::numeric_limits<std::uint64_t>::max()
                ? 0U
                : next_body_playback_revision + 1U;
    } else if (command == body_pcm_command::stop) {
        entry.playback.reset();
    }

    logical_version = plan->logical_version;
    return body_playback_receipt{
        .body = body_receipt{.identity = plan->identity,
            .accepted_sequence = request.sequence,
            .logical_version = plan->logical_version,
            .intent = plan->intent},
        .playback = entry.playback,
    };
}

result<authority::implementation::body_playback_commit_plan>
authority::implementation::prepare_body_playback_commit_locked(record& entry,
    const body_request& request,
    const body_submission_time timing,
    const body_pcm_command command,
    const prepared_body_pcm& replacement) {
    const auto evaluation_time = effective_time(timing.entered_at);
    static_cast<void>(expire_target(entry.identity.target, evaluation_time));
    if (policy_detail::is_expired(timing.expires_at, evaluation_time)) {
        return std::unexpected{error::expired};
    }
    const auto valid_lease = validate_body_lease_locked(entry, request);
    if (!valid_lease.has_value()) {
        return std::unexpected{valid_lease.error()};
    }

    const bool new_lease = !request.lease.valid();
    const body_lease_id lease = new_lease ? body_lease_id{next_body_lease_id} : request.lease;
    if (!lease.valid()) {
        return std::unexpected{error::identity_exhausted};
    }
    const body_lease_identity identity{.context = entry.identity,
        .lease = lease,
        .audio = request.audio};
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

    auto intent = planned;
    intent.identity = identity;
    intent.revision = logical_version + 1U;
    if (entry.body.has_value() && entry.body->intent.identity == identity &&
        entry.body->intent.route == intent.route &&
        entry.body->intent.fallback == intent.fallback) {
        intent.revision = entry.body->intent.revision;
    }

    body_playback_commit_plan plan{.identity = identity,
        .intent = intent,
        .logical_version = logical_version + 1U,
        .new_lease = new_lease,
        .install_selection = std::nullopt,
        .previous_playback = std::nullopt};
    if (command == body_pcm_command::replace) {
        const auto replacement_plan =
            prepare_body_playback_replacement_locked(entry, request, replacement, plan);
        if (!replacement_plan.has_value()) {
            return std::unexpected{replacement_plan.error()};
        }
    }

    return plan;
}

result<void> authority::implementation::prepare_body_playback_replacement_locked(record& entry,
    const body_request& request,
    const prepared_body_pcm& replacement,
    body_playback_commit_plan& plan) const {
    if (!replacement.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (next_body_playback_revision == 0U) {
        return std::unexpected{error::identity_exhausted};
    }

    std::optional<detail::body_playback_slot_index> current_slot;
    const auto previous_playback = entry.playback.value_or(body_playback_identity{});
    if (request.lease.valid() && previous_playback.lease == plan.identity) {
        current_slot = playback_bank.find_current(plan.identity);
    }
    plan.previous_playback = current_slot.has_value()
                                 ? std::optional<body_playback_identity>{previous_playback}
                                 : std::nullopt;
    plan.install_selection =
        playback_bank.select_install_slot(plan.identity.context.target, current_slot);
    if (!plan.install_selection.has_value()) {
        return std::unexpected{error::capacity_exceeded};
    }
    plan.replacement_identity = body_playback_identity{
        .lease = plan.identity,
        .clip_revision = next_body_playback_revision,
    };
    return {};
}

} // namespace inputnext::output
