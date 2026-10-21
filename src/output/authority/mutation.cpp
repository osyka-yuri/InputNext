#include "state.hpp"

#include "output/policy/policy.hpp"
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>

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
[[nodiscard]] result<mutation_receipt> submit_common(State& state,
    context_identity identity,
    const submit_request& request,
    owner_class kind,
    const internal_context_options* options = nullptr) {
    if (!state.valid_context_identity(identity, kind) ||
        (kind == owner_class::trusted_internal &&
            (options == nullptr || identity.internal_owner != options->owner))) {
        return std::unexpected{error::stale_context};
    }

    const auto entry_time = state.now();
    const bool trusted = kind == owner_class::trusted_internal;
    const auto permission = trusted ? options->ttl : ttl_permission::finite_only;
    const auto prevalidated =
        trusted ? policy_detail::validate_internal_submit_request(request, permission)
                : policy_detail::validate_submit_request(request);
    if (!prevalidated.has_value()) {
        return std::unexpected{prevalidated.error()};
    }

    auto deadline = policy_detail::make_deadline(entry_time, request.ttl, permission);
    if (!deadline.has_value()) {
        return std::unexpected{deadline.error()};
    }

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
        return state.commit_submit_locked(*entry, request, *deadline, entry_time);
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
    return state.commit_submit_locked(entry, request, *deadline, entry_time);
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
    policy_detail::deadline entry_time) noexcept {
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
    const auto reservations_after =
        reservations_before - static_cast<std::size_t>(removed) + static_cast<std::size_t>(added);
    if (!ordinary_headroom(reservations_after)) {
        return std::unexpected{error::logical_version_exhausted};
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
    logical_version = commit_order;
    return make_receipt(entry, commit_order);
}

result<mutation_receipt> authority::submit(public_context context, const submit_request& request) {
    return submit_common(*implementation_, context.identity, request, owner_class::public_game);
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
