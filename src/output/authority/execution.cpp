#include "execution.hpp"

#include "output/projection/represented.hpp"
#include "state.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/error.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>

namespace inputnext::output::detail {
namespace {

[[nodiscard]] execution_error map_snapshot_error(error value) noexcept {
    switch (value) {
    case error::not_connected:
        return execution_error::not_connected;
    case error::invalid_argument:
        return execution_error::invalid_argument;
    default:
        return execution_error::resources;
    }
}

} // namespace

execution_result<execution_owner> authority_execution_access::attach(authority& owner) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (state.attached_execution.has_value()) {
        return std::unexpected{execution_error::already_attached};
    }
    if (state.next_execution_id == 0U) {
        return std::unexpected{execution_error::identity_exhausted};
    }

    const execution_owner attached{.authority = state.incarnation,
        .id = execution_id{state.next_execution_id}};
    if (state.next_execution_id == std::numeric_limits<std::uint64_t>::max()) {
        state.next_execution_id = 0U;
    } else {
        ++state.next_execution_id;
    }
    state.attached_execution = attached;
    return attached;
}

execution_result<void> authority_execution_access::detach(authority& owner,
    const execution_owner expected_owner) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (!state.attached_execution.has_value()) {
        return std::unexpected{execution_error::not_running};
    }
    if (!expected_owner.valid() || *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    state.attached_execution.reset();
    return {};
}

execution_result<void> authority_execution_access::check_target(authority& owner,
    const execution_owner expected_owner,
    const connection_target target) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;
    if (!expected_owner.valid() || !state.attached_execution.has_value() ||
        *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!target.valid()) {
        return std::unexpected{execution_error::invalid_argument};
    }
    if (target.runtime != state.runtime) {
        return std::unexpected{execution_error::not_connected};
    }

    auto pin = state.capture(target);
    if (!pin.has_value()) {
        if (pin.error() == inputnext::error::not_connected) {
            static_cast<void>(state.retire(target));
            return std::unexpected{execution_error::not_connected};
        }
        return std::unexpected{execution_error::resources};
    }
    pin->release();
    return {};
}

execution_result<bool> authority_execution_access::qualifies(authority& owner,
    const execution_owner expected_owner,
    const represented_projection& expected) {
    std::scoped_lock lock{owner.implementation_->gate};
    auto& state = *owner.implementation_;

    // Reject a foreign or stale execution owner before evaluating time or maintaining records.
    if (!expected_owner.valid() || !state.attached_execution.has_value() ||
        *state.attached_execution != expected_owner) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (!expected.target.valid() || expected.target.runtime != state.runtime ||
        expected.incarnation != expected_owner.authority || expected.channels == 0U ||
        (expected.channels & ~supported_channel_mask) != 0U) {
        return std::unexpected{execution_error::invalid_argument};
    }

    auto current = state.qualify_snapshot_locked(expected.target);
    if (!current.has_value()) {
        return std::unexpected{map_snapshot_error(current.error())};
    }
    auto projected = project_representation(*current, expected.channels);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    return *projected == expected;
}

} // namespace inputnext::output::detail
