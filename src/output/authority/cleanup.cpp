#include "state.hpp"

#include "output/policy/policy.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/focus.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>

namespace inputnext::output {

void authority::implementation::advance_cleanup_version() noexcept {
    assert(logical_version != std::numeric_limits<std::uint64_t>::max());
    if (logical_version == std::numeric_limits<std::uint64_t>::max()) {
        std::terminate();
    }
    ++logical_version;
}

void authority::implementation::install_close_receipt(record& entry,
    std::uint64_t close_version,
    close_reason reason) noexcept {
    entry.state = phase::closed_retained;
    entry.receipt = close_receipt{
        .identity = entry.identity,
        .close_request_id = entry.identity.id,
        .logical_revocation_version = close_version,
        .reason = reason,
    };
    for (auto& value : entry.channels) {
        value.active = false;
        value.expires_at.reset();
    }
}

std::size_t authority::implementation::retire(connection_target target) noexcept {
    std::size_t open_removed = 0U;
    std::size_t removed = 0U;
    for (const auto& entry : records) {
        if (entry.state == phase::free || entry.identity.target != target) {
            continue;
        }
        open_removed += static_cast<std::size_t>(entry.state == phase::open);
        ++removed;
    }
    if (open_removed != 0U) {
        advance_cleanup_version();
    }
    for (auto& entry : records) {
        if (entry.state != phase::free && entry.identity.target == target) {
            entry.reset();
        }
    }
    return removed;
}

std::size_t authority::implementation::expire_target(connection_target target,
    policy_detail::deadline evaluation_time) noexcept {
    std::size_t expired = 0U;
    for (const auto& entry : records) {
        if (entry.state != phase::open || entry.identity.target != target) {
            continue;
        }
        expired += static_cast<std::size_t>(
            std::ranges::count_if(entry.channels, [evaluation_time](const channel_state& value) {
                return value.active && policy_detail::is_expired(value.expires_at, evaluation_time);
            }));
    }
    if (expired == 0U) {
        return 0U;
    }

    advance_cleanup_version();
    for (auto& entry : records) {
        if (entry.state != phase::open || entry.identity.target != target) {
            continue;
        }
        for (auto& value : entry.channels) {
            if (value.active && policy_detail::is_expired(value.expires_at, evaluation_time)) {
                value.active = false;
                value.expires_at.reset();
            }
        }
    }
    return expired;
}

bool authority::implementation::close_record(record& entry, close_reason reason) noexcept {
    if (entry.state != phase::open) {
        return false;
    }
    advance_cleanup_version();
    install_close_receipt(entry, logical_version, reason);
    return true;
}

std::size_t authority::implementation::close_bound_mismatches(connection_target target,
    std::optional<focus::foreground_epoch> observed,
    close_reason reason) noexcept {
    const auto needs_close = [target, observed](const record& entry) {
        return entry.state == phase::open && entry.identity.target == target &&
               entry.foreground == foreground_policy::foreground_bound &&
               (!observed.has_value() || entry.opening_epoch != observed);
    };
    const auto closed = static_cast<std::size_t>(std::ranges::count_if(records, needs_close));
    if (closed == 0U) {
        return 0U;
    }

    advance_cleanup_version();
    for (auto& entry : records) {
        if (needs_close(entry)) {
            install_close_receipt(entry, logical_version, reason);
        }
    }
    return closed;
}

} // namespace inputnext::output
