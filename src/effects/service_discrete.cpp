#include "implementation.hpp"

#include "backend_observation.hpp"
#include "error_mapping.hpp"
#include "identity.hpp"

#include "effects/effects.hpp"

#include "gameinput/output/connections/manager.hpp"
#include <inputnext/device_identity.hpp>
#include <inputnext/output.hpp>
#include <inputnext/output_authority.hpp>
#include <inputnext/output_submission.hpp>

#include <chrono>
#include <expected>
#include <mutex>
#include <new>

namespace inputnext::effects {

result<discrete_receipt> service::submit(const context_handle context,
    const output::submit_request& request,
    const output::finite_submission_window& window) {
    auto& state = *implementation_;
    if (request.ttl <= std::chrono::milliseconds::zero() || request.ttl != window.ttl()) {
        return std::unexpected{status::invalid_argument};
    }

    connection_target target{};
    {
        std::scoped_lock lock{state.gate};
        if (!state.accepting) {
            return std::unexpected{status::unavailable};
        }
        const auto found = state.find_open(context);
        if (!found.has_value()) {
            return std::unexpected{found.error()};
        }
        const auto* record = found.value();
        target = record->target;
    }

    gameinput_detail::managed_backend_observation backend{};
    const auto supported = current_backend_support(state.dependencies.connections,
        target,
        request.selected_channels,
        backend);
    if (supported != status::ok) {
        return std::unexpected{supported};
    }

    std::scoped_lock lock{state.gate};
    if (!state.accepting) {
        return std::unexpected{status::unavailable};
    }
    const auto found = state.find_open(context);
    if (!found.has_value()) {
        return std::unexpected{found.error()};
    }
    auto* record = found.value();

    const auto publication = state.dependencies.identities.take_publication();
    if (!publication.has_value()) {
        return std::unexpected{publication.error()};
    }

    try {
        const auto accepted =
            state.dependencies.authority.submit(record->native_context, request, window);
        if (!accepted.has_value()) {
            return std::unexpected{map_output_error(accepted.error())};
        }
        if (accepted->identity != record->native_context.identity ||
            accepted->accepted_sequence != request.sequence || accepted->logical_version == 0U) {
            return std::unexpected{status::io_failed};
        }

        record->latest_discrete = discrete_marker{.present = true,
            .publication = *publication,
            .selected_channels = request.selected_channels,
            .receipt = *accepted};
        return discrete_receipt{.context = context,
            .accepted_mask = request.selected_channels,
            .sequence = accepted->accepted_sequence,
            .publication = *publication,
            .channel_revisions = accepted->resulting_revisions};
    } catch (const std::bad_alloc&) {
        return std::unexpected{status::capacity_exceeded};
    } catch (...) {
        return std::unexpected{status::io_failed};
    }
}

status service::clear(const context_handle context, const output::clear_request& request) {
    auto& state = *implementation_;
    connection_target target{};
    {
        std::scoped_lock lock{state.gate};
        if (!state.accepting) {
            return status::unavailable;
        }
        const auto found = state.find_open(context);
        if (!found.has_value()) {
            return found.error();
        }
        const auto* record = found.value();
        target = record->target;
    }

    gameinput_detail::managed_backend_observation backend{};
    const auto supported = current_backend_support(state.dependencies.connections,
        target,
        request.selected_channels,
        backend);
    if (supported != status::ok) {
        return supported;
    }

    std::scoped_lock lock{state.gate};
    if (!state.accepting) {
        return status::unavailable;
    }
    const auto found = state.find_open(context);
    if (!found.has_value()) {
        return found.error();
    }
    const auto* record = found.value();

    try {
        const auto cleared = state.dependencies.authority.clear(record->native_context, request);
        return cleared.has_value() ? status::ok : map_output_error(cleared.error());
    } catch (const std::bad_alloc&) {
        return status::capacity_exceeded;
    } catch (...) {
        return status::io_failed;
    }
}

} // namespace inputnext::effects
