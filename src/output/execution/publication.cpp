#include "engine_state.hpp"

#include "engine.hpp"
#include "output/projection/represented.hpp"
#include <inputnext/output_authority.hpp>
#include <inputnext/output_execution.hpp>

#include <cstdint>
#include <expected>
#include <limits>
#include <mutex>

namespace inputnext::output::detail {

execution_result<void> execution_engine::publish(const backend_handle handle,
    const logical_snapshot& snapshot) noexcept {
    std::scoped_lock lock{implementation_->gate};
    auto& state = *implementation_;
    auto* const backend = state.find_backend(handle);
    if (backend == nullptr || backend->state != implementation::backend_state::current) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (snapshot.target != handle.target || snapshot.incarnation != state.execution.authority) {
        return std::unexpected{execution_error::invalid_argument};
    }
    if (snapshot.logical_version < backend->observed_logical_version) {
        return {};
    }

    auto projected =
        project_representation(snapshot, backend->supported_channels, backend->route_support);
    if (!projected.has_value()) {
        return std::unexpected{projected.error()};
    }
    backend->observed_logical_version = snapshot.logical_version;
    backend->unsupported_channels = snapshot.present_channels() & ~backend->supported_channels;

    if (backend->projection.has_value() && *backend->projection == *projected) {
        implementation::retain_body_ack_for(*backend, represented_body_route(*projected));
        if (backend->refresh_requested) {
            backend->refresh_requested = false;
            if (backend->delivery == delivery_state::pending && !backend->write_in_flight) {
                backend->delivery_needed = true;
                backend->due = clock::now();
            }
            state.notify_lane(backend->lane);
        }
        return {};
    }

    if (backend->next_publication_id == 0U) {
        backend->delivery = delivery_state::identity_exhausted;
        backend->delivery_needed = false;
        backend->refresh_requested = false;
        state.retire_locked(*backend, retirement_reason::identity_exhausted);
        state.notify_lane(backend->lane);
        return std::unexpected{execution_error::identity_exhausted};
    }

    backend->publication = publication_id{backend->next_publication_id};
    if (backend->next_publication_id == std::numeric_limits<std::uint64_t>::max()) {
        backend->next_publication_id = 0U;
    } else {
        ++backend->next_publication_id;
    }
    backend->projection = *projected;
    implementation::retain_body_ack_for(*backend, represented_body_route(*projected));
    backend->consecutive_retryable_failures = 0U;
    backend->delivery = delivery_state::pending;
    backend->delivery_needed = true;
    backend->refresh_requested = false;
    backend->due = clock::now();
    state.notify_lane(backend->lane);
    return {};
}

} // namespace inputnext::output::detail
