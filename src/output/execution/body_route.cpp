#include "engine_state.hpp"

#include "engine.hpp"
#include "output/authority/execution.hpp"
#include <inputnext/native_output.hpp>
#include <inputnext/output_body.hpp>
#include <inputnext/output_execution.hpp>

#include <expected>
#include <mutex>
#include <optional>

namespace inputnext::output::detail {

void execution_engine::implementation::retain_body_ack_for(backend_record& backend,
    const std::optional<body_route_intent>& route) noexcept {
    if (!backend.acknowledged_body_route.has_value()) {
        return;
    }
    const auto& acknowledgement = *backend.acknowledged_body_route;
    if (!route.has_value() || route->route != body_actuator_route::hd ||
        acknowledgement.backend != backend.handle || acknowledgement.intent != *route) {
        backend.acknowledged_body_route.reset();
    }
}

execution_result<completion_disposition> execution_engine::implementation::complete_body_route(
    backend_record& backend,
    const native_command& command,
    const native_result result) noexcept {
    if (backend.route_support != body_route_support::hd || !command.output.body.has_value() ||
        command.output.body->route != body_actuator_route::hd) {
        return completion_disposition::current;
    }

    const auto completion = authority_execution_access::complete_route(owner,
        execution,
        *command.output.body,
        result.status);
    if (!completion.has_value()) {
        backend.acknowledged_body_route.reset();
        return std::unexpected{completion.error()};
    }
    if (*completion != completion_disposition::current ||
        result.status != native_status::accepted || backend.retry_requested) {
        backend.acknowledged_body_route.reset();
        return completion;
    }

    retain_body_ack_for(backend, command.output.body);
    if (!backend.acknowledged_body_route.has_value()) {
        backend.acknowledged_body_route = body_route_ack{.backend = command.backend,
            .publication = command.publication,
            .attempt = command.attempt,
            .intent = *command.output.body};
    }
    return completion;
}

execution_result<bool> execution_engine::qualifies_route(
    const body_route_ack& acknowledgement) const noexcept {
    std::scoped_lock lock{implementation_->gate};
    const auto& state = *implementation_;
    const auto* const backend = state.find_backend(acknowledgement.backend);
    if (backend == nullptr) {
        return std::unexpected{execution_error::stale_backend};
    }
    if (backend->state != implementation::backend_state::current ||
        !backend->acknowledged_body_route.has_value() ||
        *backend->acknowledged_body_route != acknowledgement || backend->retry_requested ||
        backend->route_support != body_route_support::hd ||
        acknowledgement.intent.route != body_actuator_route::hd ||
        !acknowledgement.publication.valid() || !acknowledgement.attempt.valid()) {
        return false;
    }

    return authority_execution_access::qualifies_route(state.owner,
        state.execution,
        acknowledgement.intent);
}

} // namespace inputnext::output::detail
