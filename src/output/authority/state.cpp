#include "state.hpp"

#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/device_registry.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <stdexcept>

namespace inputnext::output {

authority::implementation::implementation(runtime_id runtime_value,
    device_registry& registry_value,
    focus::observer& focus_value,
    authority_seed seed_value,
    authority_incarnation incarnation_value)
    : runtime(runtime_value),
      incarnation(incarnation_value),
      registry(registry_value),
      focus_observer(focus_value),
      next_context_id(seed_value.next_context_id),
      logical_version(seed_value.logical_version),
      last_evaluation_time(policy_detail::deadline::min()) {
    if (!runtime.valid() || next_context_id == 0U || !incarnation.valid()) {
        throw std::invalid_argument{"output authority requires valid runtime and context seed"};
    }
}

policy_detail::deadline authority::implementation::now() const noexcept {
    if (test_clock_ticks != nullptr) {
        using duration = policy_detail::clock::duration;
        using rep = duration::rep;
        return policy_detail::deadline{
            duration{static_cast<rep>(test_clock_ticks->load(std::memory_order_acquire))}};
    }
    return policy_detail::clock::now();
}

policy_detail::deadline authority::implementation::effective_time(
    policy_detail::deadline lower_bound) noexcept {
    last_evaluation_time = std::max({last_evaluation_time, lower_bound, now()});
    return last_evaluation_time;
}

std::size_t authority::implementation::open_count() const noexcept {
    return static_cast<std::size_t>(std::ranges::count_if(records,
        [](const record& entry) { return entry.state == phase::open; }));
}

std::size_t authority::implementation::active_finite_count() const noexcept {
    std::size_t count = 0U;
    for (const auto& entry : records) {
        if (entry.state != phase::open) {
            continue;
        }
        count += static_cast<std::size_t>(
            std::ranges::count_if(entry.channels, [](const channel_state& value) {
                return value.active && value.expires_at.has_value();
            }));
    }
    return count;
}

std::size_t authority::implementation::reservation_count() const noexcept {
    return open_count() + active_finite_count();
}

bool authority::implementation::valid_context_identity(context_identity identity,
    owner_class expected_class) const noexcept {
    if (!identity.incarnation.valid() || identity.incarnation != incarnation ||
        !identity.target.valid() || identity.target.runtime != runtime || !identity.id.valid() ||
        identity.kind != expected_class) {
        return false;
    }

    switch (expected_class) {
    case owner_class::public_game:
        return !identity.internal_owner.valid();
    case owner_class::trusted_internal:
        return identity.internal_owner.valid();
    }
    return false;
}

bool authority::implementation::matches_grant(const record& entry,
    const internal_context_options& options) noexcept {
    return entry.identity.kind == owner_class::trusted_internal &&
           entry.identity.internal_owner == options.owner && entry.priority == options.priority &&
           entry.ttl == options.ttl && entry.foreground == options.foreground;
}

bool authority::implementation::existing_internal_grant_conflicts(connection_target target,
    const internal_context_options& options) const noexcept {
    for (const auto& entry : records) {
        if (entry.state == phase::open && entry.identity.kind == owner_class::trusted_internal &&
            entry.identity.target == target && entry.identity.internal_owner == options.owner) {
            return !matches_grant(entry, options);
        }
    }
    return false;
}

authority::implementation::record* authority::implementation::find_open(
    context_identity identity) noexcept {
    const auto found = std::ranges::find_if(records, [identity](const record& entry) {
        return entry.state == phase::open && entry.identity == identity;
    });
    return found == records.end() ? nullptr : &*found;
}

authority::implementation::record* authority::implementation::find_retained(
    context_identity identity) noexcept {
    const auto found = std::ranges::find_if(records, [identity](const record& entry) {
        return entry.state == phase::closed_retained && entry.identity == identity;
    });
    return found == records.end() ? nullptr : &*found;
}

authority::implementation::record* authority::implementation::find_free(owner_class kind) noexcept {
    const std::size_t begin = kind == owner_class::public_game ? 0U : 32U;
    const std::size_t end = begin + 32U;
    for (std::size_t index = begin; index < end; ++index) {
        if (records[index].state == phase::free) {
            return &records[index];
        }
    }
    return nullptr;
}

bool authority::implementation::ordinary_headroom(std::size_t reservations_after) const noexcept {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto available = maximum - logical_version;
    return available >= static_cast<std::uint64_t>(reservations_after) + 1U;
}

result<std::uint64_t> authority::implementation::take_context_id() noexcept {
    if (next_context_id == 0U) {
        return std::unexpected{error::identity_exhausted};
    }

    const auto value = next_context_id;
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        next_context_id = 0U;
    } else {
        ++next_context_id;
    }
    return value;
}

} // namespace inputnext::output
