#include "state.hpp"

#include "incarnation.hpp"
#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/error.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>

namespace inputnext::output {
namespace {

template <typename State>
[[nodiscard]] std::size_t sweep_retired(State& state) {
    std::array<connection_target, 64U> visited{};
    std::size_t visited_count = 0U;
    std::size_t reclaimed = 0U;
    for (const auto& entry : state.records) {
        if (!entry.identity.id.valid()) {
            continue;
        }
        const auto target = entry.identity.target;
        const auto duplicate = std::ranges::find(visited.begin(),
            visited.begin() + static_cast<std::ptrdiff_t>(visited_count),
            target);
        if (duplicate != visited.begin() + static_cast<std::ptrdiff_t>(visited_count)) {
            continue;
        }
        visited[visited_count] = target;
        ++visited_count;
        auto pin = state.capture(target);
        if (!pin.has_value() && pin.error() == inputnext::error::not_connected) {
            reclaimed += state.retire(target);
        }
    }
    return reclaimed;
}

template <typename State>
[[nodiscard]] result<void> capture_target(State& state, connection_target target) {
    auto pin = state.capture(target);
    if (!pin.has_value()) {
        if (pin.error() == inputnext::error::not_connected) {
            static_cast<void>(state.retire(target));
            return std::unexpected{error::not_connected};
        }
        return std::unexpected{error::resources};
    }
    pin->release();
    return {};
}

template <typename State>
[[nodiscard]] result<std::uint64_t> prepare_context(State& state) {
    if (!state.ordinary_headroom(state.reservation_count() + 1U)) {
        return std::unexpected{error::logical_version_exhausted};
    }
    return state.take_context_id();
}

} // namespace

namespace {

void validate_authority_construction(runtime_id runtime, authority_seed seed) {
    if (!runtime.valid() || seed.next_context_id == 0U) {
        throw std::invalid_argument{"output authority requires valid runtime and context seed"};
    }
}

} // namespace

authority::authority(runtime_id runtime,
    device_registry& registry,
    focus::observer& focus_observer,
    authority_seed seed)
    : authority(runtime, registry, focus_observer, seed, detail::incarnation_creation::generate) {
}

authority::authority(runtime_id runtime,
    device_registry& registry,
    focus::observer& focus_observer,
    authority_seed seed,
    detail::incarnation_creation creation) {
    validate_authority_construction(runtime, seed);
    const auto incarnation = detail::create_authority_incarnation(creation);
    implementation_ =
        std::make_unique<implementation>(runtime, registry, focus_observer, seed, incarnation);
}

authority::~authority() noexcept = default;

result<public_context> authority::open(connection_target target) {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (target.runtime != implementation_->runtime) {
        return std::unexpected{error::not_connected};
    }

    const auto entry_time = implementation_->now();
    std::scoped_lock lock{implementation_->gate};
    const auto evaluation_time = implementation_->effective_time(entry_time);
    static_cast<void>(implementation_->expire_target(target, evaluation_time));

    const auto before = implementation_->fresh_epoch();
    static_cast<void>(implementation_->close_bound_mismatches(target,
        before,
        close_reason::foreground_revocation));
    if (!before.has_value()) {
        return std::unexpected{error::revoked};
    }
    auto captured = capture_target(*implementation_, target);
    if (!captured.has_value()) {
        return std::unexpected{captured.error()};
    }
    const auto after = implementation_->fresh_epoch();
    if (after != before) {
        static_cast<void>(implementation_->close_bound_mismatches(target,
            std::nullopt,
            close_reason::foreground_revocation));
        return std::unexpected{error::revoked};
    }

    auto* free = implementation_->find_free(owner_class::public_game);
    if (free == nullptr) {
        static_cast<void>(sweep_retired(*implementation_));
        free = implementation_->find_free(owner_class::public_game);
    }
    if (free == nullptr) {
        return std::unexpected{error::capacity_exceeded};
    }
    const auto new_identity = prepare_context(*implementation_);
    if (!new_identity.has_value()) {
        return std::unexpected{new_identity.error()};
    }

    free->state = implementation::phase::open;
    free->identity = context_identity{
        .target = target,
        .incarnation = implementation_->incarnation,
        .id = context_id{*new_identity},
        .kind = owner_class::public_game,
        .internal_owner = {},
    };
    free->priority = internal_priority::game_semantic;
    free->ttl = ttl_permission::finite_only;
    free->foreground = foreground_policy::foreground_bound;
    free->opening_epoch = before;
    ++implementation_->logical_version;
    return public_context{free->identity};
}

result<internal_context> authority::open_internal(connection_target target,
    const internal_context_options& options) {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (target.runtime != implementation_->runtime) {
        return std::unexpected{error::not_connected};
    }
    const auto valid_options = policy_detail::validate_internal_context_options(options);
    if (!valid_options.has_value()) {
        return std::unexpected{valid_options.error()};
    }

    const auto entry_time = implementation_->now();
    std::scoped_lock lock{implementation_->gate};
    if (implementation_->existing_internal_grant_conflicts(target, options)) {
        return std::unexpected{error::invalid_argument};
    }
    const auto evaluation_time = implementation_->effective_time(entry_time);
    static_cast<void>(implementation_->expire_target(target, evaluation_time));

    std::optional<focus::foreground_epoch> before;
    if (options.foreground == foreground_policy::foreground_bound) {
        before = implementation_->fresh_epoch();
        static_cast<void>(implementation_->close_bound_mismatches(target,
            before,
            close_reason::foreground_revocation));
        if (!before.has_value()) {
            return std::unexpected{error::revoked};
        }
    }
    auto captured = capture_target(*implementation_, target);
    if (!captured.has_value()) {
        return std::unexpected{captured.error()};
    }
    if (options.foreground == foreground_policy::foreground_bound) {
        const auto after = implementation_->fresh_epoch();
        if (after != before) {
            static_cast<void>(implementation_->close_bound_mismatches(target,
                std::nullopt,
                close_reason::foreground_revocation));
            return std::unexpected{error::revoked};
        }
    }

    for (auto const& entry : implementation_->records) {
        if (entry.state != implementation::phase::open ||
            entry.identity.kind != owner_class::trusted_internal ||
            entry.identity.target != target || entry.identity.internal_owner != options.owner) {
            continue;
        }
        if (entry.priority != options.priority || entry.ttl != options.ttl ||
            entry.foreground != options.foreground) {
            return std::unexpected{error::invalid_argument};
        }
        return internal_context{entry.identity};
    }

    auto* free = implementation_->find_free(owner_class::trusted_internal);
    if (free == nullptr) {
        static_cast<void>(sweep_retired(*implementation_));
        free = implementation_->find_free(owner_class::trusted_internal);
    }
    if (free == nullptr) {
        return std::unexpected{error::capacity_exceeded};
    }
    const auto new_identity = prepare_context(*implementation_);
    if (!new_identity.has_value()) {
        return std::unexpected{new_identity.error()};
    }

    free->state = implementation::phase::open;
    free->identity = context_identity{
        .target = target,
        .incarnation = implementation_->incarnation,
        .id = context_id{*new_identity},
        .kind = owner_class::trusted_internal,
        .internal_owner = options.owner,
    };
    free->priority = options.priority;
    free->ttl = options.ttl;
    free->foreground = options.foreground;
    free->opening_epoch = before;
    ++implementation_->logical_version;
    return internal_context{free->identity};
}

result<context_info> authority::inspect(public_context context) {
    if (!implementation_->valid_context_identity(context.identity, owner_class::public_game)) {
        return std::unexpected{error::stale_context};
    }
    std::scoped_lock lock{implementation_->gate};
    const auto evaluation_time = implementation_->effective_time();
    static_cast<void>(implementation_->expire_target(context.identity.target, evaluation_time));
    auto* const entry = implementation_->find_open(context.identity);
    if (entry == nullptr) {
        return std::unexpected{implementation_->find_retained(context.identity) != nullptr
                                   ? error::revoked
                                   : error::stale_context};
    }

    context_info info;
    info.identity = entry->identity;
    info.accepted_sequence = entry->accepted_sequence;
    info.priority = entry->priority;
    info.foreground = entry->opening_epoch;
    for (std::size_t index = 0U; index < channel_count; ++index) {
        info.revisions[index] = entry->channels[index].revision;
    }
    return info;
}

result<close_receipt> authority::implementation::close_with_retry(context_identity identity,
    owner_class expected_class,
    std::uint64_t close_request_id,
    const internal_context_options* options) {
    if (!valid_context_identity(identity, expected_class)) {
        return std::unexpected{error::stale_context};
    }
    if (expected_class == owner_class::trusted_internal &&
        (options == nullptr || identity.internal_owner != options->owner)) {
        return std::unexpected{error::stale_context};
    }
    if (close_request_id != identity.id.value) {
        return std::unexpected{error::invalid_argument};
    }

    std::scoped_lock lock{gate};
    auto* entry = find_open(identity);
    if (entry == nullptr) {
        entry = find_retained(identity);
    }
    if (entry == nullptr) {
        return std::unexpected{error::stale_context};
    }
    if (expected_class == owner_class::trusted_internal && !matches_grant(*entry, *options)) {
        return std::unexpected{error::stale_context};
    }
    if (entry->state == phase::closed_retained && entry->receipt.has_value()) {
        return entry->receipt.value_or(close_receipt{});
    }
    if (!close_record(*entry, close_reason::explicit_close)) {
        return std::unexpected{error::stale_context};
    }
    if (!entry->receipt.has_value()) {
        return std::unexpected{error::stale_context};
    }
    return entry->receipt.value_or(close_receipt{});
}

result<close_receipt> authority::close(public_context context, std::uint64_t close_request_id) {
    return implementation_->close_with_retry(context.identity,
        owner_class::public_game,
        close_request_id);
}

result<close_receipt> authority::close_internal(internal_context context,
    std::uint64_t close_request_id,
    const internal_context_options& options) {
    return implementation_->close_with_retry(context.identity,
        owner_class::trusted_internal,
        close_request_id,
        &options);
}

result<void> authority::acknowledge(const close_receipt& receipt) {
    const auto& identity = receipt.identity;
    if (!implementation_->valid_context_identity(identity, owner_class::public_game) ||
        receipt.close_request_id != identity.id) {
        return std::unexpected{error::stale_context};
    }
    std::scoped_lock lock{implementation_->gate};
    auto* const entry = implementation_->find_retained(identity);
    if (entry == nullptr || !entry->receipt.has_value() || *entry->receipt != receipt) {
        return std::unexpected{error::stale_context};
    }
    entry->reset();
    return {};
}

result<void> authority::acknowledge_internal(const close_receipt& receipt,
    const internal_context_options& options) {
    const auto& identity = receipt.identity;
    if (!implementation_->valid_context_identity(identity, owner_class::trusted_internal) ||
        identity.internal_owner != options.owner || receipt.close_request_id != identity.id) {
        return std::unexpected{error::stale_context};
    }
    std::scoped_lock lock{implementation_->gate};
    auto* const entry = implementation_->find_retained(identity);
    if (entry == nullptr || !implementation::matches_grant(*entry, options) ||
        !entry->receipt.has_value() || *entry->receipt != receipt) {
        return std::unexpected{error::stale_context};
    }
    entry->reset();
    return {};
}

result<maintenance_report> authority::maintain(connection_target target) {
    if (!target.valid()) {
        return std::unexpected{error::invalid_argument};
    }
    if (target.runtime != implementation_->runtime) {
        return std::unexpected{error::not_connected};
    }
    std::scoped_lock lock{implementation_->gate};
    const auto evaluation_time = implementation_->effective_time();
    maintenance_report report{
        .target = target,
        .incarnation = implementation_->incarnation,
        .logical_version = implementation_->logical_version,
        .expired_channels = implementation_->expire_target(target, evaluation_time),
        .evaluated_at = evaluation_time,
    };
    report.closed_contexts = implementation_->close_bound_mismatches(target,
        implementation_->fresh_epoch(),
        close_reason::foreground_revocation);

    auto pin = implementation_->capture(target);
    if (!pin.has_value()) {
        if (pin.error() == inputnext::error::not_connected) {
            const auto final_time = implementation_->effective_time(evaluation_time);
            report.expired_channels += implementation_->expire_target(target, final_time);
            report.evaluated_at = final_time;
            report.reclaimed_contexts = implementation_->retire(target);
            report.logical_version = implementation_->logical_version;
            return report;
        }
        return std::unexpected{error::resources};
    }
    pin->release();
    const auto final_time = implementation_->effective_time(evaluation_time);
    report.expired_channels += implementation_->expire_target(target, final_time);
    report.evaluated_at = final_time;
    report.logical_version = implementation_->logical_version;
    return report;
}

} // namespace inputnext::output
