#include "backend_observation.hpp"

#include "error_mapping.hpp"

#include "effects/effects.hpp"
#include "gameinput/output/connections/backend_selection.hpp"
#include "gameinput/output/connections/manager.hpp"

#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_execution.hpp>

#include <expected>
#include <new>
#include <optional>

namespace inputnext::effects {

status current_backend(gameinput_detail::gameinput_output_connections& connections,
    const connection_target target,
    gameinput_detail::managed_backend_observation& out) noexcept {
    const auto observed = [&]
        -> output::execution_result<std::optional<gameinput_detail::managed_backend_observation>> {
        try {
            return connections.observe_backend(target);
        } catch (const std::bad_alloc&) {
            return std::unexpected{output::execution_error::capacity_exceeded};
        } catch (...) {
            return std::unexpected{output::execution_error::pending};
        }
    }();
    if (!observed.has_value()) {
        return map_execution_error(observed.error());
    }
    if (!observed->has_value()) {
        return status::unsupported_target;
    }
    out = **observed;
    if (out.kind == gameinput_detail::output_backend_kind::none ||
        out.execution.progress.backend.target != target ||
        out.execution.progress.phase != output::backend_phase::current ||
        !out.execution.progress.backend.valid()) {
        return status::unsupported_target;
    }
    return status::ok;
}

status current_backend_support(gameinput_detail::gameinput_output_connections& connections,
    const connection_target target,
    const output::channel_mask requested,
    gameinput_detail::managed_backend_observation& out) noexcept {
    const auto observed = current_backend(connections, target, out);
    if (observed != status::ok) {
        return observed;
    }
    if (requested == 0U || (requested & ~output::supported_channel_mask) != 0U) {
        return status::invalid_argument;
    }
    if ((out.execution.progress.represented_channels & requested) != requested) {
        return status::unsupported_channel;
    }
    return status::ok;
}

} // namespace inputnext::effects
