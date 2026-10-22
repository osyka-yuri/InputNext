#include "state.hpp"

#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/input_binding.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

namespace inputnext::output {
namespace detail {

bool foreground_pair_qualifies(std::optional<focus::foreground_epoch> before,
    std::optional<focus::foreground_epoch> after,
    std::span<const std::optional<focus::foreground_epoch>> opening_epochs) noexcept {
    return before.has_value() && before == after &&
           std::ranges::all_of(opening_epochs,
               [before](const auto& opening_epoch) { return opening_epoch == before; });
}

} // namespace detail

namespace {

template <typename Record>
struct candidate_batch final {
    std::array<policy_detail::candidate_view, policy_detail::maximum_candidate_count> candidates{};
    std::array<Record*, policy_detail::maximum_candidate_count> owners{};
    std::array<std::size_t, policy_detail::maximum_candidate_count> channel_indices{};
    std::size_t count{};
    policy_detail::optional_deadline earliest_expiry;
};

template <typename State>
[[nodiscard]] auto collect_candidates(State& state,
    connection_target target,
    policy_detail::deadline evaluation_time,
    bool permit_bound,
    std::optional<focus::foreground_epoch> epoch) {
    using record_type = std::remove_reference_t<decltype(state.records[0U])>;
    candidate_batch<record_type> batch;
    for (auto& entry : state.records) {
        if (entry.state != State::phase::open || !entry.identity.id.valid() ||
            entry.identity.target != target) {
            continue;
        }
        const bool eligible = entry.foreground == foreground_policy::background ||
                              (permit_bound && epoch.has_value() && entry.opening_epoch == epoch);
        if (!eligible) {
            continue;
        }
        for (std::size_t index = 0U; index < channel_count; ++index) {
            auto& value = entry.channels[index];
            if (!value.active || policy_detail::is_expired(value.expires_at, evaluation_time)) {
                continue;
            }
            if (value.expires_at.has_value() && (!batch.earliest_expiry.has_value() ||
                                                    *value.expires_at < *batch.earliest_expiry)) {
                batch.earliest_expiry = value.expires_at;
            }
            if (batch.count >= batch.candidates.size()) {
                return result<candidate_batch<record_type>>{std::unexpected{error::resources}};
            }
            batch.candidates[batch.count] = policy_detail::candidate_view{
                .source_channel = static_cast<channel>(index),
                .priority = entry.priority,
                .accepted_commit_order = value.accepted_commit_order,
                .expires_at = value.expires_at,
            };
            batch.owners[batch.count] = &entry;
            batch.channel_indices[batch.count] = index;
            ++batch.count;
        }
    }
    return result<candidate_batch<record_type>>{std::move(batch)};
}

template <typename Record>
[[nodiscard]] result<void> project_winners(logical_snapshot& snapshot,
    const candidate_batch<Record>& batch,
    const policy_detail::winner_indices& winners) noexcept {
    for (std::size_t index = 0U; index < channel_count; ++index) {
        if (!winners[index].has_value()) {
            continue;
        }
        const auto selected_index = winners[index].value_or(batch.count);
        if (selected_index >= batch.count) {
            return std::unexpected{error::resources};
        }
        auto& owner = *batch.owners[selected_index];
        const auto channel_index = batch.channel_indices[selected_index];
        const auto source_channel = static_cast<channel>(channel_index);
        const auto& value = owner.channels[channel_index];
        const channel_provenance provenance{
            .identity = owner.identity,
            .source_channel = source_channel,
            .revision = value.revision,
            .accepted_sequence = value.accepted_sequence,
            .accepted_commit_order = value.accepted_commit_order,
            .priority = owner.priority,
            .foreground = owner.opening_epoch,
        };
        switch (source_channel) {
        case channel::left_trigger:
            snapshot.left_trigger =
                channel_snapshot<trigger_value>{.value = owner.accepted_values.left_trigger,
                    .provenance = provenance};
            break;
        case channel::right_trigger:
            snapshot.right_trigger =
                channel_snapshot<trigger_value>{.value = owner.accepted_values.right_trigger,
                    .provenance = provenance};
            break;
        case channel::rumble:
            snapshot.rumble = channel_snapshot<rumble_value>{.value = owner.accepted_values.rumble,
                .provenance = provenance};
            break;
        case channel::leds:
            snapshot.leds = channel_snapshot<led_value>{.value = owner.accepted_values.leds,
                .provenance = provenance};
            break;
        }
    }
    return {};
}

} // namespace

result<logical_snapshot> authority::implementation::compose_snapshot(connection_target target,
    policy_detail::deadline evaluation_time,
    bool permit_bound,
    std::optional<focus::foreground_epoch> epoch) noexcept {
    const auto final_evaluation_time = effective_time(evaluation_time);
    static_cast<void>(expire_target(target, final_evaluation_time));
    logical_snapshot snapshot;
    snapshot.target = target;
    snapshot.incarnation = incarnation;
    snapshot.logical_version = logical_version;
    snapshot.evaluated_at = final_evaluation_time;
    const auto collected =
        collect_candidates(*this, target, final_evaluation_time, permit_bound, epoch);
    if (!collected.has_value()) {
        return std::unexpected{collected.error()};
    }
    snapshot.earliest_expiry = collected->earliest_expiry;
    const auto winners = policy_detail::select_winners(
        std::span<const policy_detail::candidate_view>{collected->candidates.data(),
            collected->count},
        final_evaluation_time);
    if (!winners.has_value()) {
        return std::unexpected{winners.error()};
    }
    const auto projected = project_winners(snapshot, *collected, *winners);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    return snapshot;
}

result<logical_snapshot> authority::implementation::finish_snapshot_locked(connection_target target,
    policy_detail::deadline evaluation_time,
    std::optional<focus::foreground_epoch> before,
    std::optional<focus::foreground_epoch> after,
    inputnext::read_pin first_pin) noexcept {
    std::array<std::optional<focus::foreground_epoch>, 64U> opening_epochs{};
    std::size_t opening_epoch_count = 0U;
    for (const auto& entry : records) {
        if (entry.state == phase::open && entry.identity.target == target &&
            entry.foreground == foreground_policy::foreground_bound) {
            opening_epochs[opening_epoch_count] = entry.opening_epoch;
            ++opening_epoch_count;
        }
    }
    const bool stable = detail::foreground_pair_qualifies(before,
        after,
        std::span<const std::optional<focus::foreground_epoch>>{opening_epochs.data(),
            opening_epoch_count});
    if (stable) {
        first_pin.release();
        return compose_snapshot(target, evaluation_time, true, before);
    }

    static_cast<void>(
        close_bound_mismatches(target, std::nullopt, close_reason::foreground_revocation));
    first_pin.release();
    auto fallback_pin = capture(target);
    if (!fallback_pin.has_value()) {
        if (fallback_pin.error() == inputnext::error::not_connected) {
            static_cast<void>(retire(target));
            return std::unexpected{error::not_connected};
        }
        return std::unexpected{error::resources};
    }
    fallback_pin->release();
    return compose_snapshot(target, evaluation_time, false, std::nullopt);
}

result<logical_snapshot> authority::implementation::qualify_snapshot_locked(
    connection_target target) {
    const auto evaluation_time = effective_time();
    static_cast<void>(expire_target(target, evaluation_time));
    const auto before = fresh_epoch();
    static_cast<void>(close_bound_mismatches(target, before, close_reason::foreground_revocation));

    const auto has_bound = [this, target] {
        return std::ranges::any_of(records, [target](const auto& entry) {
            return entry.state == phase::open && entry.identity.target == target &&
                   entry.foreground == foreground_policy::foreground_bound;
        });
    };

    if (!has_bound()) {
        auto pin = capture(target);
        if (!pin.has_value()) {
            if (pin.error() == inputnext::error::not_connected) {
                static_cast<void>(retire(target));
                return std::unexpected{error::not_connected};
            }
            return std::unexpected{error::resources};
        }
        pin->release();
        return compose_snapshot(target, evaluation_time, false, std::nullopt);
    }

    auto first_pin = capture(target);
    if (!first_pin.has_value()) {
        if (first_pin.error() == inputnext::error::not_connected) {
            static_cast<void>(retire(target));
            return std::unexpected{error::not_connected};
        }
        return std::unexpected{error::resources};
    }
    const auto after = fresh_epoch();
    return finish_snapshot_locked(target, evaluation_time, before, after, std::move(*first_pin));
}

result<logical_snapshot> authority::read_snapshot(connection_target target) {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (target.runtime != implementation_->runtime) {
        return std::unexpected{error::not_connected};
    }

    std::scoped_lock lock{implementation_->gate};
    return implementation_->qualify_snapshot_locked(target);
}

} // namespace inputnext::output
