#include "state.hpp"

#include "output/policy/policy.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_submission.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>
#include <utility>

namespace inputnext::output {
namespace {

template <typename Record>
[[nodiscard]] std::uint64_t selected_finite_count(const Record& entry,
    channel_mask selected) noexcept {
    std::uint64_t result_count = 0U;
    for (std::size_t index = 0U; index < channel_count; ++index) {
        const auto mask = mask_for(static_cast<channel>(index));
        if ((selected & mask) != 0U && entry.channels[index].active &&
            entry.channels[index].expires_at.has_value()) {
            ++result_count;
        }
    }
    return result_count;
}

template <typename Values>
void assign_selected_values(Values& stored,
    const channel_values& incoming,
    channel_mask selected) noexcept {
    if ((selected & left_trigger_mask) != 0U) {
        stored.left_trigger = incoming.left_trigger;
    }
    if ((selected & right_trigger_mask) != 0U) {
        stored.right_trigger = incoming.right_trigger;
    }
    if ((selected & rumble_mask) != 0U) {
        stored.rumble = incoming.rumble;
    }
    if ((selected & leds_mask) != 0U) {
        stored.leds = incoming.leds;
    }
}

template <typename Record>
[[nodiscard]] mutation_receipt make_receipt(const Record& entry, std::uint64_t version) noexcept {
    mutation_receipt receipt;
    receipt.identity = entry.identity;
    receipt.accepted_sequence = entry.accepted_sequence;
    receipt.logical_version = version;
    for (std::size_t index = 0U; index < channel_count; ++index) {
        receipt.resulting_revisions[index] = entry.channels[index].revision;
    }
    return receipt;
}

template <typename State>
[[nodiscard]] auto get_open_record(State& state, context_identity identity, owner_class kind) {
    using record_pointer = decltype(state.records.data());
    if (!state.valid_context_identity(identity, kind)) {
        return result<record_pointer>{std::unexpected{error::stale_context}};
    }

    auto* entry = state.find_open(identity);
    if (entry == nullptr) {
        const auto failure =
            state.find_retained(identity) != nullptr ? error::revoked : error::stale_context;
        return result<record_pointer>{std::unexpected{failure}};
    }
    return result<record_pointer>{entry};
}

template <typename State, typename Record>
[[nodiscard]] result<void> qualify_mutation(State& state, Record& entry) {
    return state.qualify_mutation(entry, entry.identity.target, entry.opening_epoch);
}

template <typename State>
[[nodiscard]] auto prepare_submission_timing(State& state,
    const submit_request& request,
    owner_class kind,
    const internal_context_options* options,
    const finite_submission_window* window)
    -> result<std::pair<policy_detail::deadline, policy_detail::optional_deadline>> {
    if (window != nullptr && request.ttl != window->ttl()) {
        return std::unexpected{error::invalid_argument};
    }
    const auto entry_time = window == nullptr ? state.now() : window->entered_at();
    const bool trusted = kind == owner_class::trusted_internal;
    const auto permission = trusted ? options->ttl : ttl_permission::finite_only;
    const auto prevalidated =
        trusted ? policy_detail::validate_internal_submit_request(request, permission)
                : policy_detail::validate_submit_request(request);
    if (!prevalidated.has_value()) {
        return std::unexpected{prevalidated.error()};
    }

    policy_detail::optional_deadline deadline_value{};
    if (window != nullptr) {
        deadline_value = window->expires_at();
    } else {
        const auto deadline = policy_detail::make_deadline(entry_time, request.ttl, permission);
        if (!deadline.has_value()) {
            return std::unexpected{deadline.error()};
        }
        deadline_value = *deadline;
    }

    return std::pair{entry_time, deadline_value};
}

template <typename State>
[[nodiscard]] result<mutation_receipt> submit_common(State& state,
    context_identity identity,
    const submit_request& request,
    owner_class kind,
    const internal_context_options* options = nullptr,
    const finite_submission_window* window = nullptr) {
    if (!state.valid_context_identity(identity, kind) ||
        (kind == owner_class::trusted_internal &&
            (options == nullptr || identity.internal_owner != options->owner))) {
        return std::unexpected{error::stale_context};
    }
    const auto timing = prepare_submission_timing(state, request, kind, options, window);
    if (!timing.has_value()) {
        return std::unexpected{timing.error()};
    }
    const auto entry_time = timing->first;
    const auto deadline_value = timing->second;
    const bool trusted = kind == owner_class::trusted_internal;

    std::scoped_lock lock{state.gate};

    if (trusted) {
        auto* entry = state.find_open(identity);
        if (entry == nullptr) {
            entry = state.find_retained(identity);
        }
        if (entry == nullptr || !state.matches_grant(*entry, *options)) {
            return std::unexpected{error::stale_context};
        }
        const auto evaluation_time = state.effective_time(entry_time);
        static_cast<void>(state.expire_target(identity.target, evaluation_time));
        if (entry->state != State::phase::open) {
            return std::unexpected{error::revoked};
        }
        const auto qualified = qualify_mutation(state, *entry);
        if (!qualified.has_value()) {
            return std::unexpected{qualified.error()};
        }
        return state.commit_submit_locked(*entry, request, deadline_value, entry_time, window);
    }

    const auto evaluation_time = state.effective_time(entry_time);
    static_cast<void>(state.expire_target(identity.target, evaluation_time));
    auto found = get_open_record(state, identity, kind);
    if (!found.has_value()) {
        return std::unexpected{found.error()};
    }
    auto& entry = **found;
    const auto qualified = qualify_mutation(state, entry);
    if (!qualified.has_value()) {
        return std::unexpected{qualified.error()};
    }
    return state.commit_submit_locked(entry, request, deadline_value, entry_time, window);
}

template <typename State, typename Record>
[[nodiscard]] result<mutation_receipt>
commit_clear(State& state, Record& entry, const clear_request& request) {
    if (request.sequence == 0U || request.sequence <= entry.accepted_sequence) {
        return std::unexpected{error::sequence_rejected};
    }
    for (std::size_t index = 0U; index < channel_count; ++index) {
        if ((request.selected_channels & mask_for(static_cast<channel>(index))) == 0U) {
            continue;
        }
        if (entry.channels[index].revision != request.expected_revisions[index]) {
            return std::unexpected{error::stale_revision};
        }
        if (entry.channels[index].revision == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected{error::revision_exhausted};
        }
    }

    const auto removed = selected_finite_count(entry, request.selected_channels);
    const auto reservations_after = state.reservation_count() - static_cast<std::size_t>(removed);
    if (!state.ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
    }

    const auto commit_order = state.logical_version + 1U;
    for (std::size_t index = 0U; index < channel_count; ++index) {
        if ((request.selected_channels & mask_for(static_cast<channel>(index))) == 0U) {
            continue;
        }
        auto& value = entry.channels[index];
        ++value.revision;
        value.accepted_sequence = request.sequence;
        value.accepted_commit_order = commit_order;
        value.active = false;
        value.expires_at.reset();
    }
    entry.accepted_sequence = request.sequence;
    state.logical_version = commit_order;
    return make_receipt(entry, commit_order);
}

template <typename State>
[[nodiscard]] result<mutation_receipt> clear_common(State& state,
    context_identity identity,
    const clear_request& request,
    owner_class kind,
    const internal_context_options* options = nullptr) {
    if (!state.valid_context_identity(identity, kind) ||
        (kind == owner_class::trusted_internal &&
            (options == nullptr || identity.internal_owner != options->owner))) {
        return std::unexpected{error::stale_context};
    }

    const auto prevalidated = policy_detail::validate_clear_request(request);
    if (!prevalidated.has_value()) {
        return std::unexpected{prevalidated.error()};
    }
    const auto entry_time = state.now();
    std::scoped_lock lock{state.gate};

    if (kind == owner_class::trusted_internal) {
        auto* entry = state.find_open(identity);
        if (entry == nullptr) {
            entry = state.find_retained(identity);
        }
        if (entry == nullptr || options == nullptr || !state.matches_grant(*entry, *options)) {
            return std::unexpected{error::stale_context};
        }
        const auto evaluation_time = state.effective_time(entry_time);
        static_cast<void>(state.expire_target(identity.target, evaluation_time));
        if (entry->state != State::phase::open) {
            return std::unexpected{error::revoked};
        }
        const auto qualified = qualify_mutation(state, *entry);
        if (!qualified.has_value()) {
            return std::unexpected{qualified.error()};
        }
        return commit_clear(state, *entry, request);
    }

    const auto evaluation_time = state.effective_time(entry_time);
    static_cast<void>(state.expire_target(identity.target, evaluation_time));
    auto found = get_open_record(state, identity, kind);
    if (!found.has_value()) {
        return std::unexpected{found.error()};
    }
    auto& entry = **found;
    const auto qualified = qualify_mutation(state, entry);
    if (!qualified.has_value()) {
        return std::unexpected{qualified.error()};
    }
    return commit_clear(state, entry, request);
}

} // namespace

result<mutation_receipt> authority::implementation::commit_submit_locked(record& entry,
    const submit_request& request,
    policy_detail::optional_deadline deadline,
    policy_detail::deadline entry_time,
    const finite_submission_window* window) noexcept {
    const auto evaluation_time = effective_time(entry_time);
    static_cast<void>(expire_target(entry.identity.target, evaluation_time));
    if (policy_detail::is_expired(deadline, evaluation_time)) {
        return std::unexpected{error::expired};
    }
    if (request.sequence == 0U || request.sequence <= entry.accepted_sequence) {
        return std::unexpected{error::sequence_rejected};
    }

    for (std::size_t index = 0U; index < channel_count; ++index) {
        if ((request.selected_channels & mask_for(static_cast<channel>(index))) != 0U &&
            entry.channels[index].revision == std::numeric_limits<std::uint64_t>::max()) {
            return std::unexpected{error::revision_exhausted};
        }
    }

    const auto removed = selected_finite_count(entry, request.selected_channels);
    const auto added = deadline.has_value()
                           ? static_cast<std::uint64_t>(
                                 std::popcount(request.selected_channels & supported_channel_mask))
                           : 0U;
    const auto reservations_before = reservation_count();
    const bool mutates_diagnostic_rumble = entry.priority == internal_priority::diagnostic &&
                                           (request.selected_channels & rumble_mask) != 0U;

    const auto preempted_hd_bodies =
        mutates_diagnostic_rumble ? hd_body_count(entry.identity.target) : 0U;
    const auto reservations_after = reservations_before - static_cast<std::size_t>(removed) +
                                    static_cast<std::size_t>(added) - preempted_hd_bodies;
    if (!ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
    }

    if (window != nullptr &&
        policy_detail::is_expired(policy_detail::optional_deadline{window->expires_at()},
            effective_time())) {
        return std::unexpected{error::expired};
    }

    const auto commit_order = logical_version + 1U;
    assign_selected_values(entry.accepted_values, request.values, request.selected_channels);
    for (std::size_t index = 0U; index < channel_count; ++index) {
        if ((request.selected_channels & mask_for(static_cast<channel>(index))) == 0U) {
            continue;
        }
        auto& value = entry.channels[index];
        ++value.revision;
        value.accepted_sequence = request.sequence;
        value.accepted_commit_order = commit_order;
        value.active = true;
        value.expires_at = deadline;
    }
    entry.accepted_sequence = request.sequence;
    if (mutates_diagnostic_rumble) {
        static_cast<void>(
            latch_hd_bodies(entry.identity.target, body_fallback_reason::preempted, commit_order));
    }
    logical_version = commit_order;
    return make_receipt(entry, commit_order);
}

result<mutation_receipt> authority::submit(public_context context, const submit_request& request) {
    return submit_common(*implementation_, context.identity, request, owner_class::public_game);
}

result<mutation_receipt> authority::submit(public_context context,
    const submit_request& request,
    const finite_submission_window& window) {
    return submit_common(*implementation_,
        context.identity,
        request,
        owner_class::public_game,
        nullptr,
        &window);
}

result<mutation_receipt> authority::clear(public_context context, const clear_request& request) {
    return clear_common(*implementation_, context.identity, request, owner_class::public_game);
}

result<mutation_receipt> authority::submit_internal(internal_context context,
    const submit_request& request,
    const internal_context_options& options) {
    return submit_common(*implementation_,
        context.identity,
        request,
        owner_class::trusted_internal,
        &options);
}

result<mutation_receipt> authority::clear_internal(internal_context context,
    const clear_request& request,
    const internal_context_options& options) {
    return clear_common(*implementation_,
        context.identity,
        request,
        owner_class::trusted_internal,
        &options);
}

} // namespace inputnext::output
