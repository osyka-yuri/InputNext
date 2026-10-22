#include "engine_state.hpp"

#include "engine.hpp"
#include "output/authority/execution.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/native_output.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

namespace inputnext::output::detail {

execution_result<const execution_engine::implementation::backend_record*>
execution_engine::implementation::validate_expected_backend(const connection_target target,
    const std::optional<backend_handle> expected) const noexcept {
    if (!expected.has_value()) {
        return static_cast<backend_record*>(nullptr);
    }
    const auto* const backend = find_backend(*expected);
    if (backend == nullptr || backend->state != backend_state::current) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (target.runtime != expected->target.runtime || target.device != expected->target.device) {
        return std::unexpected{execution_error::invalid_argument};
    }
    return backend;
}

execution_result<execution_engine::implementation::lane_selection>
execution_engine::implementation::select_lane(const connection_target target,
    const bool replacing,
    const backend_record* const expected_backend) noexcept {
    const auto lane_index = find_lane(target.runtime, target.device);
    if (lane_index.has_value()) {
        auto const& lane = lanes[*lane_index];
        if (lane.phase == lane_phase::starting || lane.phase == lane_phase::closing) {
            return std::unexpected{execution_error::busy};
        }
        const auto* const current = find_current(lane.token);
        if (replacing) {
            if (current != expected_backend) {
                return std::unexpected{execution_error::stale_backend};
            }
        } else if (current != nullptr) {
            return std::unexpected{execution_error::backend_conflict};
        } else if (current_lane_count() >= execution_current_lane_capacity) {
            return std::unexpected{execution_error::capacity_exceeded};
        }
        return lane_selection{.slot = static_cast<std::uint16_t>(*lane_index)};
    }

    if (replacing) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (current_lane_count() >= execution_current_lane_capacity) {
        return std::unexpected{execution_error::capacity_exceeded};
    }
    const auto free_lane = std::ranges::find_if(lanes,
        [](const auto& lane) { return lane.phase == lane_phase::free; });
    if (free_lane == lanes.end()) {
        return std::unexpected{execution_error::capacity_exceeded};
    }
    const auto slot = static_cast<std::uint16_t>(std::distance(lanes.begin(), free_lane));
    return lane_selection{.slot = slot, .start_worker = true};
}

execution_result<registration_reservation> execution_engine::implementation::reserve_selected_lane(
    const connection_target target,
    const channel_mask supported_channels,
    const std::optional<backend_handle> expected,
    const lane_selection selection) noexcept {
    const auto free_backend = std::ranges::find_if(backends,
        [](const auto& backend) { return backend.state == backend_state::free; });
    if (free_backend == backends.end()) {
        return std::unexpected{execution_error::capacity_exceeded};
    }
    if (next_backend_id == 0U || (selection.start_worker && next_lane_generation == 0U)) {
        return std::unexpected{execution_error::identity_exhausted};
    }

    auto& lane = lanes[selection.slot];
    if (selection.start_worker) {
        lane.token = lane_token{.slot = selection.slot, .generation = next_lane_generation};
        if (next_lane_generation == std::numeric_limits<std::uint64_t>::max()) {
            next_lane_generation = 0U;
        } else {
            ++next_lane_generation;
        }
        lane.runtime = target.runtime;
        lane.device = target.device;
        lane.phase = lane_phase::starting;
        lane.shutdown_requested = false;
    }

    const backend_id reserved_backend{next_backend_id};
    if (next_backend_id == std::numeric_limits<std::uint64_t>::max()) {
        next_backend_id = 0U;
    } else {
        ++next_backend_id;
    }

    auto& backend = *free_backend;
    backend = backend_record{};
    backend.state = backend_state::reserved;
    backend.id = reserved_backend;
    backend.lane = lane.token;
    backend.reserved_target = target;
    backend.supported_channels = supported_channels;
    backend.expected_backend = expected;

    return registration_reservation{.reserved_backend = reserved_backend,
        .lane = lane.token,
        .target = target,
        .supported_channels = supported_channels,
        .expected_backend = expected,
        .start_worker = selection.start_worker};
}

execution_result<registration_reservation> execution_engine::reserve_backend(
    const connection_target target,
    const std::unique_ptr<native_sink>& candidate,
    const channel_mask supported_channels,
    const std::optional<backend_handle> expected) {
    if (!target.valid() || candidate == nullptr || supported_channels == 0U ||
        (supported_channels & ~supported_channel_mask) != 0U) {
        return std::unexpected{execution_error::invalid_argument};
    }

    std::scoped_lock lock{implementation_->gate};
    auto& state = *implementation_;
    if (state.closed) {
        return std::unexpected{execution_error::not_running};
    }
    auto expected_backend = state.validate_expected_backend(target, expected);
    if (!expected_backend.has_value()) {
        return std::unexpected{expected_backend.error()};
    }
    auto lane = state.select_lane(target, expected.has_value(), *expected_backend);
    if (!lane.has_value()) {
        return std::unexpected{lane.error()};
    }
    return state.reserve_selected_lane(target, supported_channels, expected, *lane);
}

execution_result<void> execution_engine::worker_started(const lane_token token) noexcept {
    std::scoped_lock lock{implementation_->gate};
    auto* const lane = implementation_->find_lane(token);
    if (lane == nullptr || lane->phase != implementation::lane_phase::starting) {
        return std::unexpected{execution_error::stale_backend};
    }
    lane->phase = implementation::lane_phase::running;
    implementation_->notify_lane(token);
    return {};
}

execution_result<void> execution_engine::worker_start_failed(const lane_token token) noexcept {
    std::scoped_lock lock{implementation_->gate};
    auto* const lane = implementation_->find_lane(token);
    if (lane == nullptr || lane->phase != implementation::lane_phase::starting) {
        return std::unexpected{execution_error::stale_backend};
    }
    for (auto& backend : implementation_->backends) {
        if (backend.state == implementation::backend_state::reserved && backend.lane == token) {
            backend = implementation::backend_record{};
        }
    }
    if (implementation_->lane_has_records(token)) {
        return std::unexpected{execution_error::resources};
    }
    lane->phase = implementation::lane_phase::free;
    lane->token = {};
    lane->runtime = {};
    lane->device = {};
    lane->shutdown_requested = false;
    return {};
}

execution_result<backend_handle> execution_engine::commit_backend(
    const registration_reservation& reservation,
    std::unique_ptr<native_sink>& candidate) {
    if (!reservation.valid() || candidate == nullptr) {
        return std::unexpected{execution_error::invalid_argument};
    }

    std::scoped_lock lock{implementation_->gate};
    auto& state = *implementation_;
    if (state.closed) {
        return std::unexpected{execution_error::not_running};
    }
    auto const* const lane = state.find_lane(reservation.lane);
    auto* const backend = state.find_reserved(reservation.reserved_backend, reservation.lane);
    if (lane == nullptr || backend == nullptr || backend->reserved_target != reservation.target ||
        backend->supported_channels != reservation.supported_channels ||
        backend->expected_backend != reservation.expected_backend) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (lane->phase != implementation::lane_phase::running) {
        return std::unexpected{execution_error::busy};
    }

    implementation::backend_record* expected_record = nullptr;
    if (reservation.expected_backend.has_value()) {
        expected_record = state.find_backend(*reservation.expected_backend);
        if (expected_record == nullptr ||
            expected_record->state != implementation::backend_state::current ||
            expected_record->lane != reservation.lane ||
            state.find_current(reservation.lane) != expected_record) {
            return std::unexpected{execution_error::stale_backend};
        }
    } else if (state.find_current(reservation.lane) != nullptr) {
        return std::unexpected{execution_error::backend_conflict};
    }

    const auto target_check =
        authority_execution_access::check_target(state.owner, state.execution, reservation.target);
    if (!target_check.has_value()) {
        return std::unexpected{target_check.error()};
    }

    logical_snapshot initial{};
    initial.target = reservation.target;
    initial.incarnation = state.execution.authority;
    auto initial_projection = project_representation(initial, reservation.supported_channels);
    if (!initial_projection.has_value()) {
        return std::unexpected{initial_projection.error()};
    }

    backend_handle handle{.target = reservation.target,
        .incarnation = backend_incarnation{.owner = state.execution, .id = backend->id}};
    backend->state = implementation::backend_state::current;
    backend->handle = handle;
    backend->sink = std::move(candidate);
    backend->projection = *initial_projection;
    backend->publication = publication_id{1U};
    backend->next_publication_id = 2U;
    backend->delivery = delivery_state::pending;
    backend->delivery_needed = true;
    backend->due = clock::now();
    backend->expected_backend.reset();
    backend->reserved_target = {};

    if (expected_record != nullptr) {
        state.retire_locked(*expected_record, retirement_reason::replaced);
    }
    state.notify_coordinator_refresh();
    state.notify_lane(reservation.lane);
    return handle;
}

execution_result<void> execution_engine::cancel_backend(
    const registration_reservation& reservation) noexcept {
    if (!reservation.valid()) {
        return std::unexpected{execution_error::invalid_argument};
    }
    std::scoped_lock lock{implementation_->gate};
    auto* const backend =
        implementation_->find_reserved(reservation.reserved_backend, reservation.lane);
    if (backend == nullptr) {
        return std::unexpected{execution_error::stale_backend};
    }
    *backend = implementation::backend_record{};
    implementation_->maybe_close_lane_locked(reservation.lane);
    return {};
}

} // namespace inputnext::output::detail
