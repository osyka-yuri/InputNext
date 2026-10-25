#include "engine_state.hpp"

#include "engine.hpp"
#include "output/authority/execution.hpp"

#include <inputnext/output_body.hpp>
#include <inputnext/output_body_playback.hpp>
#include <inputnext/output_execution.hpp>

#include <cstddef>
#include <expected>
#include <limits>
#include <mutex>
#include <optional>

namespace inputnext::output::detail {

execution_result<std::optional<body_render_permit>> execution_engine::admit_body_render(
    const body_route_ack& acknowledgement,
    const body_playback_identity expected_playback,
    const std::size_t requested_frames) noexcept {
    if (requested_frames == 0U || requested_frames > std::numeric_limits<std::size_t>::max() / 4U) {
        return std::unexpected{execution_error::invalid_argument};
    }

    std::scoped_lock lock{implementation_->gate};
    auto& state = *implementation_;
    auto* const backend = state.find_backend(acknowledgement.backend);
    if (backend == nullptr) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (backend->state != implementation::backend_state::current ||
        !backend->acknowledged_body_route.has_value() ||
        *backend->acknowledged_body_route != acknowledgement || backend->retry_requested ||
        backend->route_support != body_route_support::hd ||
        acknowledgement.intent.route != body_actuator_route::hd ||
        !acknowledgement.publication.valid() || !acknowledgement.attempt.valid() ||
        !expected_playback.valid() || acknowledgement.intent.identity != expected_playback.lease) {
        return std::optional<body_render_permit>{};
    }

    const auto index = static_cast<std::size_t>(backend - state.backends.data());
    auto& backend_busy = state.render_busy[index];
    return authority_execution_access::admit_body_render(state.owner,
        state.execution,
        acknowledgement,
        expected_playback,
        &backend_busy,
        requested_frames);
}

} // namespace inputnext::output::detail
