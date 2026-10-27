#include "engine_state.hpp"

#include "drive_policy.hpp"
#include "engine.hpp"
#include "output/authority/execution.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/output_execution.hpp>
#include <inputnext/output_observation.hpp>

#include <chrono>
#include <expected>
#include <mutex>

namespace inputnext::output::detail {

backend_progress execution_engine::implementation::make_progress(
    const backend_record& backend) noexcept {
    backend_phase phase = backend_phase::current;
    switch (backend.state) {
    case backend_state::reserved:
    case backend_state::free:
        phase = backend_phase::drained;
        break;
    case backend_state::current:
        phase = backend_phase::current;
        break;
    case backend_state::retiring:
        phase = backend_phase::retiring;
        break;
    case backend_state::cleaning:
        phase = backend_phase::cleaning;
        break;
    case backend_state::drained:
        phase = backend_phase::drained;
        break;
    }

    const auto scheduled_retry_delay =
        backend.state == backend_state::current && backend.delivery == delivery_state::retry_wait &&
                backend.delivery_needed && !backend.write_in_flight && !backend.refresh_requested &&
                !backend.retry_requested
            ? drive_policy::retry_delay_for(backend.consecutive_retryable_failures)
            : std::chrono::milliseconds::zero();

    return backend_progress{.backend = backend.handle,
        .phase = phase,
        .represented_channels = backend.supported_channels,
        .unsupported_channels = backend.unsupported_channels,
        .observed_publication = backend.publication,
        .delivery = backend.delivery,
        .consecutive_retryable_failures = backend.consecutive_retryable_failures,
        .scheduled_retry_delay = scheduled_retry_delay,
        .last_attempt = backend.last_attempt,
        .last_completed_attempt = backend.last_completed_attempt,
        .write_in_flight = backend.write_in_flight,
        .retry_requested = backend.retry_requested,
        .acknowledged_body_route = backend.acknowledged_body_route,
        .body_routes = backend.route_support};
}

execution_result<lane_token> execution_engine::lane_for(
    const backend_handle handle) const noexcept {
    std::scoped_lock lock{implementation_->gate};
    const auto* const backend = implementation_->find_backend(handle);
    if (backend == nullptr) {
        return std::unexpected{execution_error::stale_backend};
    }
    return backend->lane;
}

execution_result<backend_progress> execution_engine::progress(
    const backend_handle handle) const noexcept {
    std::scoped_lock lock{implementation_->gate};
    const auto* const backend = implementation_->find_backend(handle);
    if (backend == nullptr) {
        return std::unexpected{execution_error::stale_backend};
    }

    if (backend->state == implementation::backend_state::reserved ||
        backend->state == implementation::backend_state::free) {
        return std::unexpected{execution_error::stale_backend};
    }
    return implementation::make_progress(*backend);
}

execution_result<backend_observation> execution_engine::observe(const backend_handle handle) const {
    std::scoped_lock lock{implementation_->gate};
    auto& state = *implementation_;
    auto* const backend = state.find_backend(handle);
    if (backend == nullptr || backend->state != implementation::backend_state::current) {
        return std::unexpected{execution_error::stale_backend};
    }

    const auto snapshot =
        authority_execution_access::observe(state.owner, state.execution, handle.target);
    if (!snapshot.has_value()) {
        return std::unexpected{snapshot.error()};
    }
    const auto projected =
        project_representation(*snapshot, backend->supported_channels, backend->route_support);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    const bool projection_current =
        backend->projection.has_value() && *backend->projection == *projected;

    return backend_observation{.progress = implementation::make_progress(*backend),
        .observed_logical_version = backend->observed_logical_version,
        .authority_snapshot = *snapshot,
        .projection_current = projection_current};
}

} // namespace inputnext::output::detail
