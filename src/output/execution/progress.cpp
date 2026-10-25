#include "engine_state.hpp"

#include "drive_policy.hpp"
#include "engine.hpp"
#include <inputnext/output_execution.hpp>

#include <chrono>
#include <expected>
#include <mutex>

namespace inputnext::output::detail {

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

    backend_phase phase = backend_phase::current;
    switch (backend->state) {
    case implementation::backend_state::reserved:
    case implementation::backend_state::free:
        return std::unexpected{execution_error::stale_backend};
    case implementation::backend_state::current:
        phase = backend_phase::current;
        break;
    case implementation::backend_state::retiring:
        phase = backend_phase::retiring;
        break;
    case implementation::backend_state::cleaning:
        phase = backend_phase::cleaning;
        break;
    case implementation::backend_state::drained:
        phase = backend_phase::drained;
        break;
    }

    const auto scheduled_retry_delay =
        backend->state == implementation::backend_state::current &&
                backend->delivery == delivery_state::retry_wait && backend->delivery_needed &&
                !backend->write_in_flight && !backend->refresh_requested &&
                !backend->retry_requested
            ? drive_policy::retry_delay_for(backend->consecutive_retryable_failures)
            : std::chrono::milliseconds::zero();

    return backend_progress{.backend = backend->handle,
        .phase = phase,
        .represented_channels = backend->supported_channels,
        .unsupported_channels = backend->unsupported_channels,
        .observed_publication = backend->publication,
        .delivery = backend->delivery,
        .consecutive_retryable_failures = backend->consecutive_retryable_failures,
        .scheduled_retry_delay = scheduled_retry_delay,
        .last_attempt = backend->last_attempt,
        .last_completed_attempt = backend->last_completed_attempt,
        .write_in_flight = backend->write_in_flight,
        .retry_requested = backend->retry_requested,
        .acknowledged_body_route = backend->acknowledged_body_route};
}

} // namespace inputnext::output::detail
